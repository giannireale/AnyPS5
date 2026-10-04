#include "prx/libc/include/general/VabiMacros.hpp"
#include "SceTypes.hpp"
#include <cstdlib>
#include <thread>
extern "C" {
void* APS5_VABI dlopen_nid_postfix(const char*, int);
void* APS5_VABI dlsym_nid_postfix(void*, const char*);
int APS5_VABI dlclose_nid_postfix(void*);
char* APS5_VABI dlerror_nid_postfix();
KernelModule APS5_VABI sceKernelLoadStartModule(const char*, size_t, const void*, uint32_t, const KernelLoadModuleOpt*, int*);
int APS5_VABI sceKernelStopUnloadModule(KernelModule, size_t, const void*, uint32_t, const KernelUnloadModuleOpt*, int*);
}
static void Require(bool value) { if (!value) std::abort(); }
int main(int argc, char** argv) {
    Require(argc == 2);
    Require(dlerror_nid_postfix() == nullptr);
    Require(dlopen_nid_postfix(argv[1], 0x2000) == nullptr);
    Require(dlerror_nid_postfix() != nullptr);
    Require(dlerror_nid_postfix() == nullptr);
    Require(dlopen_nid_postfix("anyps5-missing-module-for-test.prx", 2) == nullptr);
    Require(dlerror_nid_postfix() != nullptr);
    void* executable = dlopen_nid_postfix(nullptr, 2);
    Require(executable != nullptr && dlclose_nid_postfix(executable) == 0);
    void* module = dlopen_nid_postfix(argv[1], 2 | 0x100);
    Require(module != nullptr);
    using Add = int (APS5_VABI *)(int, int);
    auto add = reinterpret_cast<Add>(dlsym_nid_postfix(module, "GuestModuleAdd"));
    Require(add && add(17, 25) == 42);
#ifdef _WIN32
    using Count = int (APS5_VABI *)();
    const auto count = reinterpret_cast<Count>(dlsym_nid_postfix(module, "GuestModuleStartCount"));
    Require(count && count() == 0);
    const int payload = 1234;
    int result = 0;
    const auto started = sceKernelLoadStartModule(argv[1], sizeof(payload), &payload, 0, nullptr, &result);
    Require(started > 0 && result == payload && count() == 1);
    const auto again = sceKernelLoadStartModule(argv[1], 0, nullptr, 0, nullptr, &result);
    Require(again > 0 && result == 0 && count() == 1);
    Require(sceKernelStopUnloadModule(again, 0, nullptr, 0, nullptr, nullptr) == 0);
    Require(sceKernelStopUnloadModule(started, 0, nullptr, 0, nullptr, nullptr) == 0);
#endif
    Require(dlsym_nid_postfix(reinterpret_cast<void*>(-2), "GuestModuleAdd") == reinterpret_cast<void*>(add));
    Require(dlsym_nid_postfix(module, "missing_symbol") == nullptr);
    std::thread other([] { Require(dlerror_nid_postfix() == nullptr); });
    other.join();
    Require(dlerror_nid_postfix() != nullptr);
    Require(dlerror_nid_postfix() == nullptr);
    void* second = dlopen_nid_postfix(argv[1], 1);
    Require(second && second != module);
    Require(dlclose_nid_postfix(module) == 0);
    Require(dlsym_nid_postfix(module, "GuestModuleAdd") == nullptr);
    add = reinterpret_cast<Add>(dlsym_nid_postfix(second, "GuestModuleAdd"));
    Require(add && add(2, 3) == 5);
    Require(dlclose_nid_postfix(second) == 0);
    Require(dlclose_nid_postfix(second) == -1);
}
