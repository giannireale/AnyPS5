#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstddef>
#include "prx/libc/include/general/ExportMacros.hpp"
#ifdef _WIN32
#define MODULE_EXPORT __declspec(dllexport)
#else
#define MODULE_EXPORT __attribute__((visibility("default")))
#endif
extern "C" MODULE_EXPORT int APS5_VABI GuestModuleAdd_nid_postfix(int a, int b) {
    return a + b;
}

#ifdef _WIN32
static int startCount = 0;
extern "C" MODULE_EXPORT int APS5_VABI __aps5_module_init_nid_no_patch_cut(std::size_t size, const void* argp, void*) {
    ++startCount;
    return size == sizeof(int) && argp ? *static_cast<const int*>(argp) : -1;
}
extern "C" MODULE_EXPORT int APS5_VABI GuestModuleStartCount_nid_postfix() {
    return startCount;
}
#endif
#ifndef _WIN32
extern "C" MODULE_EXPORT int APS5_VABI GuestModuleMul_nid_no_patch(int a, int b) {
    return a * b;
}
APS5_EXPORT("GuestModuleMul#guest", GuestModuleMul_nid_no_patch);
extern "C" MODULE_EXPORT int APS5_VABI GuestModuleSub_nid_no_patch(int a, int b) {
    return a - b;
}
APS5_EXPORT("BOyBJaKwOa8#guest", GuestModuleSub_nid_no_patch);
#endif
