#ifndef DOMAIN_GUESTRUNTIME_HPP
#define DOMAIN_GUESTRUNTIME_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace Domain {

struct GuestImport {
    std::string Name;
    std::uint32_t TargetRva;
    std::uint64_t Addend;
    std::uint32_t RelocationType = 1;
};

struct GuestRuntime {
    std::string Path;
    std::vector<GuestImport> Imports;
    std::vector<std::uint32_t> InitArrayRvas;
    std::vector<std::uint32_t> FiniArrayRvas;
    std::uint32_t InitRva = 0;
    std::uint32_t FiniRva = 0;
    bool UsePlatformTlsResolver = true;
    // Loaded later through sceKernelLoadStartModule rather than needed by the executable: its
    // initializer (module_start) runs then, with the caller's arguments, instead of at startup.
    bool DeferInit = false;
    // Output names of deferred guest modules this one imports from; they start before it.
    std::vector<std::string> DeferredDependencies;
};

}

#endif
