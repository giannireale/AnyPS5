#include <cstdint>
#include <cstring>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

// The player-review dialog asks the user to rate the title on the store. There is no store here,
// so the dialog never opens: status reports it finished and the result reports a cancel.
namespace {
constexpr std::int32_t CommonDialogStatusFinished = 3;
constexpr std::int32_t CommonDialogResultUserCanceled = 1;
}

extern "C" {

std::int32_t APS5_VABI scePlayerReviewDialogInitialize(void) {
    return 0;
}

std::int32_t APS5_VABI scePlayerReviewDialogTerminate(void) {
    return 0;
}

std::int32_t APS5_VABI scePlayerReviewDialogOpen(const void* param) {
    (void)param;
    return 0;
}

std::int32_t APS5_VABI scePlayerReviewDialogUpdateStatus(void) {
    return CommonDialogStatusFinished;
}

std::int32_t APS5_VABI scePlayerReviewDialogGetStatus(void) {
    return CommonDialogStatusFinished;
}

std::int32_t APS5_VABI scePlayerReviewDialogClose(void) {
    return 0;
}

std::int32_t APS5_VABI scePlayerReviewDialogGetResult(void* result) {
    if (result == nullptr) return static_cast<std::int32_t>(0x80B80003u);
    // Leading field of the result is the common-dialog result code.
    std::int32_t code = CommonDialogResultUserCanceled;
    std::memcpy(result, &code, sizeof(code));
    return 0;
}

}
