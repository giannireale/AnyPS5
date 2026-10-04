#include "SceTypes.hpp"
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

extern "C" {
int APS5_VABI sceKernelConvertLocaltimeToUtc(int64_t local, int64_t dst, int64_t* utc, KernelTimesec* zone, int32_t* dstSeconds);
int APS5_VABI sceKernelConvertUtcToLocaltime(int64_t utc, int64_t* local, KernelTimesec* zone, uint64_t* dstSeconds);
int APS5_VABI sceKernelCreateEqueue(KernelEqueue* eq, const char* name);
int APS5_VABI sceKernelDeleteEqueue(KernelEqueue eq);
int APS5_VABI sceKernelWaitEqueue(KernelEqueue eq, KernelEvent* ev, int num, int* out, const KernelUseconds* timo);
int APS5_VABI sceKernelDeleteUserEvent(KernelEqueue eq, int id);
int APS5_VABI scePthreadMutexattrInit(PthreadMutexattr* attr);
int APS5_VABI scePthreadMutexattrDestroy(PthreadMutexattr* attr);
int APS5_VABI scePthreadMutexattrSettype(PthreadMutexattr* attr, int type);
int APS5_VABI scePthreadMutexattrSetprotocol(PthreadMutexattr* attr, int protocol);
int APS5_VABI scePthreadMutexInit(PthreadMutex* mutex, const PthreadMutexattr* attr, const char* name);
int APS5_VABI scePthreadMutexDestroy(PthreadMutex* mutex);
int APS5_VABI scePthreadMutexLock(PthreadMutex* mutex);
int APS5_VABI scePthreadMutexUnlock(PthreadMutex* mutex);
}

static constexpr int SCE_OK = 0;
static constexpr int SCE_KERNEL_ERROR_ENOENT = static_cast<int>(0x80020002);
static constexpr int SCE_KERNEL_ERROR_EBADF = static_cast<int>(0x80020009);
static constexpr int SCE_KERNEL_ERROR_EDEADLK = static_cast<int>(0x8002000B);
static constexpr int SCE_KERNEL_ERROR_EFAULT = static_cast<int>(0x8002000E);
static constexpr int SCE_KERNEL_ERROR_EINVAL = static_cast<int>(0x80020016);
static constexpr int SCE_KERNEL_ERROR_ETIMEDOUT = static_cast<int>(0x8002003C);
static constexpr int MUTEX_TYPE_ERRORCHECK = 1;
static constexpr int PRIO_NONE = 0;
static constexpr int PRIO_INHERIT = 1;
static constexpr int PRIO_PROTECT = 2;

static void Require(bool value) { if (!value) std::abort(); }

int main() {
    // The guest libc reads all 16 bytes of conversion metadata. Poison the
    // fields to catch a partial write, with a guard for the adjacent stack data.
    struct { KernelTimesec zone; uint64_t guard; } conversion{{-1, 0xffffffffu, 0xffffffffu}, 0x123456789abcdef0ull};
    constexpr int64_t sampleTime = 1767225600;
    int64_t converted = -1;
    int32_t dstSeconds = -1;
    Require(sceKernelConvertLocaltimeToUtc(sampleTime, -1, &converted, &conversion.zone, &dstSeconds) == SCE_OK);
    Require(converted == sampleTime && dstSeconds == 0);
    Require(conversion.zone.t == sampleTime && conversion.zone.west_sec == 0 && conversion.zone.dst_sec == 0);
    Require(conversion.guard == 0x123456789abcdef0ull);
    conversion.zone = {-1, 0xffffffffu, 0xffffffffu};
    uint64_t utcDstSeconds = ~uint64_t{0};
    Require(sceKernelConvertUtcToLocaltime(sampleTime, &converted, &conversion.zone, &utcDstSeconds) == SCE_OK);
    Require(converted == sampleTime && utcDstSeconds == 0);
    Require(conversion.zone.t == sampleTime && conversion.zone.west_sec == 0 && conversion.zone.dst_sec == 0);
    Require(conversion.guard == 0x123456789abcdef0ull);

    KernelEqueue eq = 0;
    Require(sceKernelCreateEqueue(&eq, "errors") == SCE_OK);
    KernelEvent event{};
    int count = -1;
    const KernelUseconds timeout = 1000;
    Require(sceKernelWaitEqueue(eq, &event, 1, &count, &timeout) == SCE_KERNEL_ERROR_ETIMEDOUT);
    Require(count == 0);
    Require(sceKernelWaitEqueue(eq, nullptr, 1, &count, &timeout) == SCE_KERNEL_ERROR_EFAULT);
    Require(sceKernelWaitEqueue(eq, &event, 0, &count, &timeout) == SCE_KERNEL_ERROR_EINVAL);
    Require(sceKernelDeleteUserEvent(eq, 7) == SCE_KERNEL_ERROR_ENOENT);
    Require(sceKernelDeleteEqueue(eq) == SCE_OK);
    Require(sceKernelDeleteEqueue(eq) == SCE_KERNEL_ERROR_EBADF);
    Require(sceKernelWaitEqueue(eq, &event, 1, &count, &timeout) == SCE_KERNEL_ERROR_EBADF);
    Require(sceKernelCreateEqueue(nullptr, "errors") == SCE_KERNEL_ERROR_EINVAL);

    PthreadMutexattr attr = nullptr;
    Require(scePthreadMutexattrInit(&attr) == SCE_OK);
    Require(scePthreadMutexattrSettype(&attr, MUTEX_TYPE_ERRORCHECK) == SCE_OK);
    Require(scePthreadMutexattrSetprotocol(&attr, PRIO_NONE) == SCE_OK);
    Require(scePthreadMutexattrSetprotocol(&attr, PRIO_INHERIT) == SCE_OK);
    bool protectionRejected = false;
    try {
        scePthreadMutexattrSetprotocol(&attr, PRIO_PROTECT);
    } catch (const std::invalid_argument&) {
        protectionRejected = true;
    }
    Require(protectionRejected);
    PthreadMutex mutex = nullptr;
    Require(scePthreadMutexInit(&mutex, &attr, nullptr) == SCE_OK);
    Require(scePthreadMutexattrDestroy(&attr) == SCE_OK);
    Require(scePthreadMutexLock(&mutex) == SCE_OK);
    Require(scePthreadMutexLock(&mutex) == SCE_KERNEL_ERROR_EDEADLK);
    Require(scePthreadMutexUnlock(&mutex) == SCE_OK);
    Require(scePthreadMutexDestroy(&mutex) == SCE_OK);
}
