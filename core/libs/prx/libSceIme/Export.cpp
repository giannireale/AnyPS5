#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {

constexpr int ErrorInvalidType = static_cast<int>(0x80bc0011u);
constexpr int ErrorInvalidOption = static_cast<int>(0x80bc0015u);
constexpr int ErrorInvalidAddress = static_cast<int>(0x80bc0031u);
constexpr uint32_t TypeNumber = 4;
constexpr uint32_t ValidOptions = 0x00007bff;
constexpr uint32_t OptionUseOver2K = 0x00004000;

std::mutex g_keyboardMutex;
std::set<int32_t> g_openKeyboards;

}

extern "C" {

int APS5_VABI sceImeClose_nid_postfix(void) {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceImeGetPanelSize(const Param* param, uint32_t* width, uint32_t* height) {
 if (!param || !width || !height) return ErrorInvalidAddress;
 if (param->type > TypeNumber) return ErrorInvalidType;
 if ((param->option & ~ValidOptions) != 0) return ErrorInvalidOption;
 const uint32_t scale = (param->option & OptionUseOver2K) != 0 ? 2 : 1;
 *width = (param->type == TypeNumber ? 370u : 793u) * scale;
 *height = (param->type == TypeNumber ? 402u : 408u) * scale;
 return 0;
}

int APS5_VABI sceImeOpen_nid_postfix(const Param* param, const ExtendedParam* extended) {
 (void)param;
 (void)extended;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceImeSetCaret(const Caret* caret) {
 (void)caret;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceImeSetText(const char16_t* text, uint32_t length) {
 (void)text;
 (void)length;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceImeSetTextGeometry(TextAreaMode mode, const TextGeometry* geometry) {
 (void)mode;
 (void)geometry;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceImeUpdate(EventHandler handler) {
 if (!handler) APS5_INVALID_ARG_EX;
 return 0;
}

}
