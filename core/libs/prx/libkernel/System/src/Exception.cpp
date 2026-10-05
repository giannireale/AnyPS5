#include <cstdint>
#include <cstddef>
#include <cstdio>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include <array>
#include <atomic>
#include <cstring>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

using GuestExceptionHandler = void (APS5_VABI*)(int signum, void* context);
constexpr int kGuestSignals = 128;
std::array<std::atomic<GuestExceptionHandler>, kGuestSignals> g_exceptionHandlers{};

// The handler's context is the console's ucontext_t: the signal mask, 0x30 reserved bytes, then the
// FreeBSD amd64 mcontext. PS5Util (Unity) reads the interrupted stack pointer at offset 0xf8.
struct GuestMcontext {
    std::uint64_t onstack, rdi, rsi, rdx, rcx, r8, r9, rax, rbx, rbp, r10, r11, r12, r13, r14, r15;
    std::uint32_t trapno;
    std::uint16_t fs, gs;
    std::uint64_t addr;
    std::uint32_t flags;
    std::uint16_t es, ds;
    std::uint64_t err, rip, cs, rflags, rsp, ss;
    std::uint64_t len, fpformat, ownedfp;
    alignas(16) std::uint8_t fpstate[512];
    std::uint64_t fsbase, gsbase, xfpustate, xfpustateLength, spare[4];
};

struct GuestUcontext {
    std::uint8_t sigmask[16];
    std::uint8_t reserved[0x30];
    GuestMcontext mcontext;
    std::uint8_t tail[0x80];
};
static_assert(offsetof(GuestUcontext, mcontext) + offsetof(GuestMcontext, rsp) == 0xf8, "guest ucontext rsp offset");

#ifdef _WIN32
void FillGuestContext(const CONTEXT& host, GuestUcontext& guest) {
    std::memset(&guest, 0, sizeof(guest));
    auto& m = guest.mcontext;
    m.rdi = host.Rdi; m.rsi = host.Rsi; m.rdx = host.Rdx; m.rcx = host.Rcx; m.r8 = host.R8; m.r9 = host.R9;
    m.rax = host.Rax; m.rbx = host.Rbx; m.rbp = host.Rbp; m.r10 = host.R10; m.r11 = host.R11; m.r12 = host.R12;
    m.r13 = host.R13; m.r14 = host.R14; m.r15 = host.R15; m.rip = host.Rip; m.cs = host.SegCs;
    m.rflags = host.EFlags; m.rsp = host.Rsp; m.ss = host.SegSs; m.len = sizeof(GuestMcontext);
    std::memcpy(m.fpstate, &host.FltSave, sizeof(m.fpstate));
}

// Windows cannot interrupt a thread blocked in a non-alertable wait (winpthreads), so the handler is
// not injected into the target: the target is suspended and stays suspended while a proxy host
// thread runs the handler as the target (scePthreadSelf() and the interrupted registers), which is
// what a signal handler observes on the console. The target resumes when the handler returns.
struct RaisedSignal {
    HANDLE target;
    Pthread thread;
    GuestExceptionHandler handler;
    int signum;
    GuestUcontext context;
    GuestUcontext* stackContext;
};

DWORD WINAPI DeliverRaisedSignal(void* parameter) {
    auto* raised = static_cast<RaisedSignal*>(parameter);
    PthreadExchangeCurrent(raised->thread);
    raised->handler(raised->signum, raised->stackContext);
    PthreadExchangeCurrent(nullptr);
    ResumeThread(raised->target);
    delete raised;
    return 0;
}
#endif

int RaiseOnThread(Pthread thread, int signum, GuestExceptionHandler handler) {
#ifdef _WIN32
    const auto native = static_cast<HANDLE>(thread->nativeHandle);
    if (native == nullptr) return SCE_KERNEL_ERROR_ESRCH;
    if (GetThreadId(native) == GetCurrentThreadId()) {
        CONTEXT self{};
        RtlCaptureContext(&self);
        GuestUcontext context;
        FillGuestContext(self, context);
        handler(signum, &context);
        return 0;
    }
    if (SuspendThread(native) == static_cast<DWORD>(-1)) return SCE_KERNEL_ERROR_ESRCH;
    CONTEXT interrupted{};
    interrupted.ContextFlags = CONTEXT_FULL | CONTEXT_FLOATING_POINT;
    if (!GetThreadContext(native, &interrupted)) {
        ResumeThread(native);
        return SCE_KERNEL_ERROR_ESRCH;
    }
    auto* raised = new RaisedSignal{native, thread, handler, signum, {}, nullptr};
    FillGuestContext(interrupted, raised->context);
    // As the console kernel does, the context goes on the interrupted stack below its red zone, and
    // the stack pointer handed to the handler covers it: a conservative collector scanning from it
    // then also sees the pointers held in the interrupted registers.
    constexpr std::uintptr_t redZone = 128;
    const auto contextAddress = (interrupted.Rsp - redZone - sizeof(GuestUcontext)) & ~static_cast<std::uintptr_t>(63);
    raised->stackContext = reinterpret_cast<GuestUcontext*>(contextAddress);
    *raised->stackContext = raised->context;
    raised->stackContext->mcontext.rsp = contextAddress;
    const HANDLE proxy = CreateThread(nullptr, 1u << 20, &DeliverRaisedSignal, raised, 0, nullptr);
    if (proxy == nullptr) {
        delete raised;
        ResumeThread(native);
        return SCE_KERNEL_ERROR_EAGAIN;
    }
    CloseHandle(proxy);
    return 0;
#else
    (void)thread;
    (void)signum;
    (void)handler;
    NotImplemented_nid_no_patch("sceKernelRaiseException: guest signal delivery");
    return 0;
#endif
}

}

extern "C" {


// Exception payloads belong to the guest C++ runtime. Keep the diagnostic
// useful without interpreting a host-incompatible exception object layout.
int APS5_VABI sceKernelInstallExceptionHandler(int signum, void* handler) {
 if (signum <= 0 || signum >= kGuestSignals || handler == nullptr) return SCE_KERNEL_ERROR_EINVAL;
 g_exceptionHandlers[signum].store(reinterpret_cast<GuestExceptionHandler>(handler), std::memory_order_release);
 return 0;
}

int APS5_VABI sceKernelRemoveExceptionHandler(int signum) {
 if (signum <= 0 || signum >= kGuestSignals) return SCE_KERNEL_ERROR_EINVAL;
 g_exceptionHandlers[signum].store(nullptr, std::memory_order_release);
 return 0;
}

int APS5_VABI sceKernelRaiseException(Pthread thread, int signum) {
 if (thread == nullptr || signum <= 0 || signum >= kGuestSignals) return SCE_KERNEL_ERROR_EINVAL;
 const auto handler = g_exceptionHandlers[signum].load(std::memory_order_acquire);
 if (handler == nullptr) return SCE_KERNEL_ERROR_EINVAL;
 return RaiseOnThread(thread, signum, handler);
}

void APS5_VABI sceKernelDebugRaiseException(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseException c1=%d c2=%d", c1, c2);
}

void APS5_VABI sceKernelDebugRaiseExceptionOnReleaseMode(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseExceptionOnReleaseMode c1=%d c2=%d", c1, c2);
}

}
