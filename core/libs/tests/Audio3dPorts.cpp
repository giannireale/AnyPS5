#include "SceTypes.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
int APS5_VABI sceAudio3dInitialize(std::int64_t reserved);
void APS5_VABI sceAudio3dGetDefaultOpenParameters(Audio3dOpenParameters* p);
int APS5_VABI sceAudio3dPortOpen(int user_id, const Audio3dOpenParameters* parameters, std::uint32_t* id);
int APS5_VABI sceAudio3dPortPush(std::uint32_t port_id, std::uint32_t blocking);
int APS5_VABI sceAudio3dPortAdvance(std::uint32_t port_id);
int APS5_VABI sceAudio3dPortGetQueueLevel(std::uint32_t port_id, std::uint32_t* queue_level, std::uint32_t* queue_available);
int APS5_VABI sceAudio3dPortSetAttribute(std::uint32_t port_id, std::uint32_t attribute_id, const void* attribute, std::size_t attribute_size);
}

namespace {

int failures = 0;

void Require(const bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "Audio3dPorts: %s\n", what);
    ++failures;
}

}

int main() {
    Audio3dOpenParameters parameters{};
    std::uint32_t port = 0;
    Require(sceAudio3dPortOpen(0, &parameters, &port) != 0, "a port opened before initialisation");

    Require(sceAudio3dInitialize(0) == 0, "the library did not initialise");
    Require(sceAudio3dInitialize(1) != 0, "a non zero reserved argument was accepted");

    sceAudio3dGetDefaultOpenParameters(&parameters);
    Require(parameters.size_this == sizeof(Audio3dOpenParameters), "the defaults do not carry their own size");
    Require(parameters.granularity > 0 && parameters.rate > 0 && parameters.queue_depth > 0, "the defaults are unusable");
    sceAudio3dGetDefaultOpenParameters(nullptr);

    Audio3dOpenParameters broken = parameters;
    broken.size_this = 8;
    Require(sceAudio3dPortOpen(0, &broken, &port) != 0, "a wrong structure size was accepted");
    broken = parameters;
    broken.queue_depth = 0;
    Require(sceAudio3dPortOpen(0, &broken, &port) != 0, "an empty queue was accepted");
    broken = parameters;
    broken.rate = 0;
    Require(sceAudio3dPortOpen(0, &broken, &port) != 0, "a zero sample rate was accepted");

    Require(sceAudio3dPortOpen(0, &parameters, &port) == 0 && port != 0, "the port did not open");

    std::uint32_t level = 0xFFFFu;
    std::uint32_t available = 0xFFFFu;
    Require(sceAudio3dPortGetQueueLevel(port, &level, &available) == 0, "the queue level was refused");
    Require(level == 0 && available == parameters.queue_depth, "a fresh port is not empty");
    Require(sceAudio3dPortGetQueueLevel(port + 100, &level, &available) != 0, "an unknown port answered");

    Require(sceAudio3dPortAdvance(port) != 0, "an empty port advanced");

    for (std::uint32_t index = 0; index < parameters.queue_depth; ++index)
        Require(sceAudio3dPortPush(port, 0) == 0, "a push inside the queue depth failed");
    Require(sceAudio3dPortGetQueueLevel(port, &level, &available) == 0 && level == parameters.queue_depth && available == 0,
            "the queue does not report itself full");
    Require(sceAudio3dPortPush(port, 0) != 0, "a non blocking push on a full queue was accepted");
    Require(sceAudio3dPortPush(port, 1) == 0, "a blocking push on a full queue failed");

    Require(sceAudio3dPortAdvance(port) == 0, "the port did not advance");
    Require(sceAudio3dPortGetQueueLevel(port, &level, nullptr) == 0 && level == parameters.queue_depth - 1,
            "advancing did not free a slot");

    const std::uint8_t payload[6] = {9, 8, 7, 6, 5, 4};
    Require(sceAudio3dPortSetAttribute(port, 1, payload, sizeof(payload)) == 0, "an attribute was refused");
    Require(sceAudio3dPortSetAttribute(port, 1, nullptr, sizeof(payload)) != 0, "a null attribute was accepted");
    Require(sceAudio3dPortSetAttribute(port, 1, payload, 0) != 0, "an empty attribute was accepted");
    Require(sceAudio3dPortSetAttribute(port + 100, 1, payload, sizeof(payload)) != 0, "an attribute reached an unknown port");

    if (failures != 0) return 1;
    std::printf("Audio3d port tests passed\n");
    return 0;
}
