#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

int APS5_VABI sceVoiceSetThreadsParams(void* params) {
 (void)params;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

}
