#include "prx/libSceVoice/include/Api.hpp"
#include "SceTypes.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>

static void Require(bool value) { if (!value) std::abort(); }

int main() {
    constexpr std::int32_t voiceActivity = 1001;
    constexpr int argumentInvalid = static_cast<int>(0x80540001);
    constexpr int notInitialized = static_cast<int>(0x8054000C);
    constexpr int portInvalid = static_cast<int>(0x80540006);
    constexpr int notSupported = static_cast<int>(0x8054000E);

    std::array<unsigned char, 3> buffer{0xa5, 0xa5, 0xa5};
    Require(sceVoiceGetPortAttr(0, voiceActivity, &buffer[1], 1) == notInitialized);

    VoiceInitParam init{};
    Require(sceVoiceInit(&init, 0) == 0);

    VoicePortParam param{};
    param.port_type = 1;
    param.volume = 1.0f;
    std::uint32_t port = 0;
    Require(sceVoiceCreatePort(&port, &param) == 0);

    Require(sceVoiceGetPortAttr(port, voiceActivity, &buffer[1], 1) == 0);
    Require(buffer[0] == 0xa5 && buffer[1] == 0 && buffer[2] == 0xa5);

    buffer = {0xa5, 0xa5, 0xa5};
    Require(sceVoiceGetPortAttr(port, voiceActivity, nullptr, 1) == argumentInvalid);
    Require(sceVoiceGetPortAttr(port, voiceActivity, &buffer[1], 0) == argumentInvalid);
    Require(sceVoiceGetPortAttr(port, voiceActivity, &buffer[1], 2) == argumentInvalid);
    Require(buffer[0] == 0xa5 && buffer[1] == 0xa5 && buffer[2] == 0xa5);

    Require(sceVoiceGetPortAttr(port, 9999, &buffer[1], 1) == notSupported);
    Require(sceVoiceGetPortAttr(0x1234, voiceActivity, &buffer[1], 1) == portInvalid);

    Require(sceVoiceDeletePort(port) == 0);
    Require(sceVoiceGetPortAttr(port, voiceActivity, &buffer[1], 1) == portInvalid);

    Require(sceVoiceEnd_nid_postfix() == 0);
    return 0;
}
