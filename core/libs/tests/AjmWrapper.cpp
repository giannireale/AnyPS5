#include "SceTypes.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
int APS5_VABI sceAjmInitialize(int64_t reserved, uint32_t* context);
int APS5_VABI sceAjmFinalize(uint32_t context);
int APS5_VABI sceAjmModuleRegister(uint32_t context, uint32_t codec, int64_t reserved);
int APS5_VABI sceAjmInstanceCreate(uint32_t context, uint32_t codec, uint64_t flags, uint32_t* instance);
int APS5_VABI sceAjmInstanceDestroy(uint32_t context, uint32_t instance);
int APS5_VABI sceAjmDecAt9ParseConfigData(const void* config_data, AjmDecAt9ConfigDataInfo* config_info);
const char* APS5_VABI sceAjmStrError(int error);
}

namespace {

constexpr std::uint32_t CodecAt9 = 1;
constexpr int InvalidInstance = static_cast<int>(0x80930003);
constexpr int InvalidParameter = static_cast<int>(0x80930005);

int failures = 0;

void Require(const bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "AjmWrapper: %s\n", what);
    ++failures;
}

}

int main() {
    std::uint32_t context = 0xFFFFFFFFu;
    Require(sceAjmInitialize(0, &context) == 0, "sceAjmInitialize did not reach the native module");
    Require(sceAjmInitialize(0, nullptr) == InvalidParameter, "a null context was accepted");
    Require(sceAjmModuleRegister(context, CodecAt9, 0) == 0, "the ATRAC9 module did not register");

    std::uint32_t instance = 0xFFFFFFFFu;
    Require(sceAjmInstanceCreate(context, CodecAt9, 0, &instance) == 0, "an ATRAC9 instance was not created");
    Require(sceAjmInstanceCreate(context, CodecAt9, 0, nullptr) == InvalidParameter, "a null instance pointer was accepted");
    Require(sceAjmInstanceDestroy(context, instance) == 0, "the instance was not destroyed");
    Require(sceAjmInstanceDestroy(context, instance) == InvalidInstance, "destroying twice was accepted");

    AjmDecAt9ConfigDataInfo info{};
    const unsigned char invalid[4] = {0, 0, 0, 0};
    Require(sceAjmDecAt9ParseConfigData(invalid, &info) == InvalidParameter, "an invalid ATRAC9 configuration was accepted");
    Require(sceAjmDecAt9ParseConfigData(nullptr, &info) == InvalidParameter, "a null configuration was accepted");

    Require(std::strcmp(sceAjmStrError(0), "SCE_OK") == 0, "sceAjmStrError does not name success");
    Require(std::strcmp(sceAjmStrError(InvalidInstance), "SCE_AJM_ERROR_INVALID_INSTANCE") == 0, "sceAjmStrError does not name a known code");
    Require(std::strcmp(sceAjmStrError(123), "SCE_AJM_ERROR_UNKNOWN") == 0, "sceAjmStrError does not report an unknown code");

    Require(sceAjmFinalize(context) == 0, "sceAjmFinalize did not reach the native module");
    if (failures != 0) return 1;
    std::printf("Ajm wrapper tests passed\n");
    return 0;
}
