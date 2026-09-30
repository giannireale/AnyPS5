#include <cstdint>
#include <cstddef>
#include <cstdio>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

// Exception payloads belong to the guest C++ runtime. Keep the diagnostic
// useful without interpreting a host-incompatible exception object layout.
int APS5_VABI sceKernelDebugWriteCppExceptionInfo(const void* info) {
 std::fprintf(stderr, "[exception] guest C++ exception info: %p\n", info);
 return 0;
}

int APS5_VABI sceKernelInstallExceptionHandler(int signum, void* handler) {
 (void)signum;
 (void)handler;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceKernelRemoveExceptionHandler(int signum) {
 (void)signum;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceKernelRaiseException(Pthread thread, int signum) {
 (void)thread;
 (void)signum;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

void APS5_VABI sceKernelDebugRaiseException(int c1, int c2) {
 (void)c1;
 (void)c2;
 NotImplemented_nid_no_patch(__func__);
}

void APS5_VABI sceKernelDebugRaiseExceptionOnReleaseMode(int c1, int c2) {
 (void)c1;
 (void)c2;
 NotImplemented_nid_no_patch(__func__);
}

}
