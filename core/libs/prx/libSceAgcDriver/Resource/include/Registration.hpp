#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_RESOURCE_INCLUDE_REGISTRATION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_RESOURCE_INCLUDE_REGISTRATION_HPP

#include "SceTypes.hpp"

extern "C" int APS5_VABI sceAgcDriverGetDefaultOwner(uint32_t* owner_handle);
extern "C" int APS5_VABI sceAgcDriverGetResourceRegistrationMaxNameLength(uint32_t* max_name_length);

#endif
