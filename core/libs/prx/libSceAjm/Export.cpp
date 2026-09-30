#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

int APS5_VABI AjmNativeInitialize(int64_t reserved, uint32_t* context);
int APS5_VABI AjmNativeFinalize(uint32_t context);
int APS5_VABI AjmNativeInstanceCreate(uint32_t context, uint32_t codec, uint64_t flags, uint32_t* instance);
int APS5_VABI AjmNativeInstanceDestroy(uint32_t context, uint32_t instance);
int APS5_VABI AjmNativeModuleRegister(uint32_t context, uint32_t codec, int64_t reserved);
int APS5_VABI AjmNativeModuleUnregister(uint32_t context, uint32_t codec);
int APS5_VABI AjmNativeMemoryRegister(uint32_t context, void* ptr, size_t pages);
int APS5_VABI AjmNativeMemoryUnregister(uint32_t context, void* ptr);
int APS5_VABI AjmNativeBatchWait(uint32_t context, uint32_t batch, uint32_t timeout, AjmBatchError* error);
int APS5_VABI AjmNativeBatchErrorDump(const AjmBatchInfo* info, AjmBatchError* error);
int APS5_VABI AjmNativeDecAt9ParseConfigData(const void* config_data, AjmDecAt9ConfigDataInfo* config_info);

int APS5_VABI sceAjmInitialize(int64_t reserved, uint32_t* context) {
    return AjmNativeInitialize(reserved, context);
}

int APS5_VABI sceAjmFinalize(uint32_t context) {
    return AjmNativeFinalize(context);
}

int APS5_VABI sceAjmInstanceCreate(uint32_t context, uint32_t codec, uint64_t flags, uint32_t* instance) {
    return AjmNativeInstanceCreate(context, codec, flags, instance);
}

int APS5_VABI sceAjmInstanceDestroy(uint32_t context, uint32_t instance) {
    return AjmNativeInstanceDestroy(context, instance);
}

int APS5_VABI sceAjmModuleRegister(uint32_t context, uint32_t codec, int64_t reserved) {
    return AjmNativeModuleRegister(context, codec, reserved);
}

int APS5_VABI sceAjmModuleUnregister(uint32_t context, uint32_t codec) {
    return AjmNativeModuleUnregister(context, codec);
}

int APS5_VABI sceAjmMemoryRegister(uint32_t context, void* ptr, size_t pages) {
    return AjmNativeMemoryRegister(context, ptr, pages);
}

int APS5_VABI sceAjmMemoryUnregister(uint32_t context, void* ptr) {
    return AjmNativeMemoryUnregister(context, ptr);
}

int APS5_VABI sceAjmBatchWait(uint32_t context, uint32_t batch, uint32_t timeout, AjmBatchError* error) {
    return AjmNativeBatchWait(context, batch, timeout, error);
}

int APS5_VABI sceAjmBatchErrorDump(const AjmBatchInfo* info, AjmBatchError* error) {
    return AjmNativeBatchErrorDump(info, error);
}

int APS5_VABI sceAjmDecAt9ParseConfigData(const void* config_data, AjmDecAt9ConfigDataInfo* config_info) {
    return AjmNativeDecAt9ParseConfigData(config_data, config_info);
}

const char* APS5_VABI sceAjmStrError(int error) {
    switch (error) {
    case 0:
        return "SCE_OK";
    case static_cast<int>(0x80930001):
        return "SCE_AJM_ERROR_MALFORMED_BATCH";
    case static_cast<int>(0x80930002):
        return "SCE_AJM_ERROR_INVALID_CONTEXT";
    case static_cast<int>(0x80930003):
        return "SCE_AJM_ERROR_INVALID_INSTANCE";
    case static_cast<int>(0x80930004):
        return "SCE_AJM_ERROR_INVALID_CODEC";
    case static_cast<int>(0x80930005):
        return "SCE_AJM_ERROR_INVALID_PARAMETER";
    case static_cast<int>(0x80930006):
        return "SCE_AJM_ERROR_OUT_OF_MEMORY";
    case static_cast<int>(0x80930007):
        return "SCE_AJM_ERROR_OUT_OF_RESOURCES";
    case static_cast<int>(0x80930008):
        return "SCE_AJM_ERROR_BUSY";
    default:
        return "SCE_AJM_ERROR_UNKNOWN";
    }
}

int APS5_VABI sceAjmBatchCancel(uint32_t context, uint32_t batch) {
    (void)context;
    (void)batch;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
