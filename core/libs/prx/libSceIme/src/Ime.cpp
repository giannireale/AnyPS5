#include <cstdint>
#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include "SceTypes.hpp"

namespace {

constexpr int SCE_IME_ERROR_BUSY = static_cast<int>(0x80BC0001);
constexpr int SCE_IME_ERROR_NOT_OPENED = static_cast<int>(0x80BC0002);
constexpr int SCE_IME_ERROR_INVALID_ADDRESS = static_cast<int>(0x80BC0004);
constexpr int SCE_IME_ERROR_INVALID_USER_ID = static_cast<int>(0x80BC000C);
constexpr int SCE_IME_ERROR_INVALID_HANDLER = static_cast<int>(0x80BC0016);
constexpr int SCE_IME_ERROR_INVALID_RESOURCE_ID = static_cast<int>(0x80BC0029);
constexpr int SCE_IME_ERROR_CONNECTION_FAILED = static_cast<int>(0x80BC0035);

constexpr std::uint32_t KeyboardStatusDisconnected = 0;
constexpr std::uint32_t KeyboardDeviceNone = 0;
constexpr std::uint32_t DefaultRepeatDelay = 400;
constexpr std::uint32_t DefaultRepeatRate = 33;
constexpr std::int32_t MinUserId = 1;

struct KeyboardListener {
    KeyboardParam param{};
    std::uint32_t mode = 0;
};

std::mutex g_lock;
std::map<std::int32_t, KeyboardListener> g_keyboards;

}

extern "C" {

void APS5_VABI sceImeParamInit(Param* param) {
    if (param == nullptr) return;
    std::memset(param, 0, sizeof(Param));
    param->user_id = -1;
}

int APS5_VABI sceImeKeyboardOpen(std::int32_t user_id, const KeyboardParam* param) {
    if (user_id < MinUserId) return SCE_IME_ERROR_INVALID_USER_ID;
    if (param == nullptr) return SCE_IME_ERROR_INVALID_ADDRESS;
    if (param->handler == nullptr) return SCE_IME_ERROR_INVALID_HANDLER;
    std::lock_guard lock(g_lock);
    if (g_keyboards.contains(user_id)) return SCE_IME_ERROR_BUSY;
    g_keyboards[user_id] = KeyboardListener{*param, 0};
    return 0;
}

int APS5_VABI sceImeKeyboardClose(std::int32_t user_id) {
    if (user_id < MinUserId) return SCE_IME_ERROR_INVALID_USER_ID;
    std::lock_guard lock(g_lock);
    return g_keyboards.erase(user_id) != 0 ? 0 : SCE_IME_ERROR_NOT_OPENED;
}

int APS5_VABI sceImeKeyboardSetMode(std::int32_t user_id, std::uint32_t mode) {
    if (user_id < MinUserId) return SCE_IME_ERROR_INVALID_USER_ID;
    std::lock_guard lock(g_lock);
    const auto entry = g_keyboards.find(user_id);
    if (entry == g_keyboards.end()) return SCE_IME_ERROR_NOT_OPENED;
    entry->second.mode = mode;
    return 0;
}

int APS5_VABI sceImeKeyboardGetResourceId(std::int32_t user_id, KeyboardResourceIdArray* resource_ids) {
    if (user_id < MinUserId) return SCE_IME_ERROR_INVALID_USER_ID;
    if (resource_ids == nullptr) return SCE_IME_ERROR_INVALID_ADDRESS;
    std::lock_guard lock(g_lock);
    if (!g_keyboards.contains(user_id)) return SCE_IME_ERROR_NOT_OPENED;
    std::memset(resource_ids, 0, sizeof(KeyboardResourceIdArray));
    resource_ids->user_id = user_id;
    return 0;
}

int APS5_VABI sceImeKeyboardGetInfo(std::uint32_t resource_id, KeyboardInfo* info) {
    if (info == nullptr) return SCE_IME_ERROR_INVALID_ADDRESS;
    if (resource_id == 0) return SCE_IME_ERROR_INVALID_RESOURCE_ID;
    std::memset(info, 0, sizeof(KeyboardInfo));
    info->device = KeyboardDeviceNone;
    info->repeat_delay = DefaultRepeatDelay;
    info->repeat_rate = DefaultRepeatRate;
    info->status = KeyboardStatusDisconnected;
    return SCE_IME_ERROR_CONNECTION_FAILED;
}

}
