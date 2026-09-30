#include "SceTypes.hpp"

#include <cstdint>
#include <cstdio>

extern "C" {
void APS5_VABI sceImeParamInit(Param* param);
int APS5_VABI sceImeKeyboardOpen(std::int32_t user_id, const KeyboardParam* param);
int APS5_VABI sceImeKeyboardClose(std::int32_t user_id);
int APS5_VABI sceImeKeyboardSetMode(std::int32_t user_id, std::uint32_t mode);
int APS5_VABI sceImeKeyboardGetResourceId(std::int32_t user_id, KeyboardResourceIdArray* resource_ids);
int APS5_VABI sceImeKeyboardGetInfo(std::uint32_t resource_id, KeyboardInfo* info);
}

namespace {

int failures = 0;

void Require(const bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "ImeKeyboard: %s\n", what);
    ++failures;
}

void Handler(void* arg, const ImeEvent* event) {
    (void)arg;
    (void)event;
}

}

int main() {
    Param param{};
    param.user_id = 1234;
    sceImeParamInit(&param);
    Require(param.user_id == -1, "the initialised parameters keep no user");
    Require(param.handler == nullptr && param.max_text_length == 0, "the initialised parameters are not clean");
    sceImeParamInit(nullptr);

    KeyboardParam keyboard{};
    Require(sceImeKeyboardOpen(1, &keyboard) != 0, "a keyboard opened without an event handler");
    keyboard.handler = Handler;
    Require(sceImeKeyboardOpen(0, &keyboard) != 0, "an invalid user was accepted");
    Require(sceImeKeyboardOpen(1, nullptr) != 0, "a null parameter was accepted");
    Require(sceImeKeyboardOpen(1, &keyboard) == 0, "the keyboard did not open");
    Require(sceImeKeyboardOpen(1, &keyboard) != 0, "the same user opened twice");
    Require(sceImeKeyboardOpen(2, &keyboard) == 0, "a second user could not open");

    Require(sceImeKeyboardSetMode(1, 1) == 0, "the mode was refused");
    Require(sceImeKeyboardSetMode(3, 1) != 0, "the mode reached a user that never opened");

    KeyboardResourceIdArray ids{};
    Require(sceImeKeyboardGetResourceId(1, &ids) == 0, "the resource list was refused");
    Require(ids.user_id == 1 && ids.resource_id[0] == 0, "a resource is reported without a device");
    Require(sceImeKeyboardGetResourceId(3, &ids) != 0, "a user that never opened has resources");
    Require(sceImeKeyboardGetResourceId(1, nullptr) != 0, "a null output was accepted");

    KeyboardInfo info{};
    Require(sceImeKeyboardGetInfo(0, &info) != 0, "the zero resource identifier was accepted");
    Require(sceImeKeyboardGetInfo(7, &info) != 0, "an absent device reported success");
    Require(info.status == 0 && info.device == 0, "the absent device is not described as disconnected");
    Require(sceImeKeyboardGetInfo(7, nullptr) != 0, "a null output was accepted");

    Require(sceImeKeyboardClose(1) == 0 && sceImeKeyboardClose(2) == 0, "the keyboards did not close");
    Require(sceImeKeyboardClose(1) != 0, "a keyboard closed twice");
    Require(sceImeKeyboardSetMode(1, 0) != 0, "a closed user still accepts a mode");

    if (failures != 0) return 1;
    std::printf("Ime keyboard tests passed\n");
    return 0;
}
