#include "SceTypes.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
int APS5_VABI sceNgs2SystemQueryBufferSize(const Ngs2SystemOption* option, Ngs2ContextBufferInfo* buffer_info);
int APS5_VABI sceNgs2SystemCreate(const Ngs2SystemOption* option, const Ngs2ContextBufferInfo* buffer_info, Ngs2Handle* handle);
int APS5_VABI sceNgs2SystemCreateWithAllocator(const Ngs2SystemOption* option, const Ngs2BufferAllocator* allocator, Ngs2Handle* handle);
int APS5_VABI sceNgs2SystemDestroy(Ngs2Handle system_handle, Ngs2ContextBufferInfo* buffer_info);
int APS5_VABI sceNgs2SystemGetInfo(Ngs2Handle system_handle, Ngs2SystemInfo* info, std::size_t info_size);
int APS5_VABI sceNgs2SystemSetGrainSamples(Ngs2Handle system_handle, std::uint32_t num_samples);
int APS5_VABI sceNgs2RackQueryBufferSize(std::uint32_t rack_id, const Ngs2RackOption* option, Ngs2ContextBufferInfo* buffer_info);
int APS5_VABI sceNgs2RackCreate(Ngs2Handle system_handle, std::uint32_t rack_id, const Ngs2RackOption* option, const Ngs2ContextBufferInfo* buffer_info, Ngs2Handle* handle);
int APS5_VABI sceNgs2RackCreateWithAllocator(Ngs2Handle system_handle, std::uint32_t rack_id, const Ngs2RackOption* option, const Ngs2BufferAllocator* allocator, Ngs2Handle* handle);
int APS5_VABI sceNgs2RackDestroy(Ngs2Handle rack_handle, Ngs2ContextBufferInfo* buffer_info);
int APS5_VABI sceNgs2RackGetInfo(Ngs2Handle rack_handle, Ngs2RackInfo* info, std::size_t info_size);
int APS5_VABI sceNgs2RackGetVoiceHandle(Ngs2Handle rack_handle, std::uint32_t voice_id, Ngs2Handle* handle);
int APS5_VABI sceNgs2RackLock(Ngs2Handle rack_handle);
int APS5_VABI sceNgs2RackUnlock(Ngs2Handle rack_handle);
int APS5_VABI sceNgs2VoiceGetState(Ngs2Handle voice_handle, Ngs2VoiceState* state, std::size_t state_size);
int APS5_VABI sceNgs2VoiceGetStateFlags(Ngs2Handle voice_handle, std::uint32_t* state_flags);
}

namespace {

int failures = 0;
int allocations = 0;
int releases = 0;

void Require(const bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "Ngs2Lifecycle: %s\n", what);
    ++failures;
}

std::int32_t Allocate(void* context) {
    auto* info = static_cast<Ngs2ContextBufferInfo*>(context);
    info->host_buffer = std::malloc(info->host_buffer_size);
    ++allocations;
    return info->host_buffer != nullptr ? 0 : -1;
}

std::int32_t Release(void* context) {
    auto* info = static_cast<Ngs2ContextBufferInfo*>(context);
    std::free(info->host_buffer);
    info->host_buffer = nullptr;
    ++releases;
    return 0;
}

Ngs2SystemOption MakeSystemOption() {
    Ngs2SystemOption option{};
    option.size = sizeof(option);
    std::strcpy(option.name, "test");
    option.max_grain_samples = 512;
    option.num_grain_samples = 256;
    option.sample_rate = 48000;
    return option;
}

Ngs2RackOption MakeRackOption(const std::uint32_t voices) {
    Ngs2RackOption option{};
    option.size = sizeof(option);
    std::strcpy(option.name, "rack-test");
    option.max_voices = voices;
    option.max_matrices = 5;
    option.max_ports = 3;
    return option;
}

void WritePs5RackOption(std::array<std::uint8_t, 200>& bytes, const std::uint32_t grain, const std::uint32_t voices) {
    bytes.fill(0);
    const std::size_t size = bytes.size();
    std::memcpy(bytes.data(), &size, sizeof(size));
    std::memcpy(bytes.data() + 8, "ps5-rack", 8);
    std::memcpy(bytes.data() + 76, &grain, sizeof(grain));
    std::memcpy(bytes.data() + 80, &voices, sizeof(voices));
}

}

int main() {
    const auto systemOption = MakeSystemOption();

    Ngs2ContextBufferInfo systemBuffer{};
    Require(sceNgs2SystemQueryBufferSize(&systemOption, &systemBuffer) == 0, "the system buffer size was refused");
    Require(systemBuffer.host_buffer_size > 0, "the system buffer size is zero");
    Require(sceNgs2SystemQueryBufferSize(&systemOption, nullptr) != 0, "a null output was accepted");

    Ngs2SystemOption badOption = systemOption;
    badOption.size = 8;
    Ngs2ContextBufferInfo scratch{};
    Require(sceNgs2SystemQueryBufferSize(&badOption, &scratch) != 0, "a wrong option size was accepted");
    badOption = systemOption;
    badOption.max_grain_samples = 4096;
    Require(sceNgs2SystemQueryBufferSize(&badOption, &scratch) != 0, "an out of range grain count was accepted");

    std::vector<std::uint8_t> storage(systemBuffer.host_buffer_size);
    systemBuffer.host_buffer = storage.data();
    Ngs2Handle system = 0;
    Require(sceNgs2SystemCreate(&systemOption, &systemBuffer, &system) == 0, "the system was not created");

    Ngs2ContextBufferInfo tooSmall = systemBuffer;
    tooSmall.host_buffer_size = 16;
    Ngs2Handle rejected = 0;
    Require(sceNgs2SystemCreate(&systemOption, &tooSmall, &rejected) != 0, "an undersized buffer was accepted");

    Ngs2SystemInfo info{};
    Require(sceNgs2SystemGetInfo(system, &info, sizeof(info)) == 0, "the system info was refused");
    Require(info.system_handle == system, "the info reports another handle");
    Require(info.num_grain_samples == 256 && info.sample_rate == 48000, "the info lost the requested settings");
    Require(info.rack_count == 0, "a fresh system already has racks");
    Require(std::strcmp(info.name, "test") == 0, "the info lost the system name");
    Require(sceNgs2SystemGetInfo(system, &info, 4) != 0, "an undersized info structure was accepted");
    Require(sceNgs2SystemGetInfo(0, &info, sizeof(info)) != 0, "an invalid system handle was accepted");

    Require(sceNgs2SystemSetGrainSamples(system, 128) == 0, "a valid grain count was refused");
    Require(sceNgs2SystemSetGrainSamples(system, 7) != 0, "a too small grain count was accepted");
    Require(sceNgs2SystemGetInfo(system, &info, sizeof(info)) == 0 && info.num_grain_samples == 128, "the grain count did not change");

    const auto rackOption = MakeRackOption(4);
    Ngs2ContextBufferInfo rackBuffer{};
    Require(sceNgs2RackQueryBufferSize(0, &rackOption, &rackBuffer) == 0, "the rack buffer size was refused");
    std::vector<std::uint8_t> rackStorage(rackBuffer.host_buffer_size);
    rackBuffer.host_buffer = rackStorage.data();
    Ngs2Handle rack = 0;
    Require(sceNgs2RackCreate(system, 42, &rackOption, &rackBuffer, &rack) == 0, "the rack was not created");
    Require(sceNgs2RackCreate(0, 0, &rackOption, &rackBuffer, &rack) != 0, "a rack on an invalid system was accepted");
    Require(sceNgs2SystemGetInfo(system, &info, sizeof(info)) == 0 && info.rack_count == 1, "the system does not count its rack");

    struct OversizedRackInfo {
        Ngs2RackInfo info;
        std::uint8_t canary[64];
    } oversizedInfo;
    std::memset(&oversizedInfo, 0xA5, sizeof(oversizedInfo));
    Require(sceNgs2RackGetInfo(rack, &oversizedInfo.info, sizeof(oversizedInfo.info)) == 0, "the rack info was refused");
    Require(std::strcmp(oversizedInfo.info.name, "rack-test") == 0, "the rack info lost its name");
    Require(oversizedInfo.info.rack_handle == rack && oversizedInfo.info.rack_id == 42, "the rack info reports another rack");
    Require(oversizedInfo.info.buffer_info.host_buffer == rackStorage.data() &&
                oversizedInfo.info.buffer_info.host_buffer_size == rackStorage.size(),
            "the rack info lost its buffer");
    Require(oversizedInfo.info.owner_system_handle == system, "the rack info reports another owner system");
    Require(oversizedInfo.info.uid != 0, "the rack info has no uid");
    Require(oversizedInfo.info.min_grain_samples == 64 && oversizedInfo.info.max_grain_samples == 512,
            "the rack info lost its grain limits");
    Require(oversizedInfo.info.max_voices == 4 && oversizedInfo.info.max_matrices == 5 && oversizedInfo.info.max_ports == 3,
            "the rack info lost its configured limits");
    bool tailUnchanged = true;
    for (const auto value : oversizedInfo.canary) tailUnchanged = tailUnchanged && value == 0xA5;
    Require(tailUnchanged, "the rack info overwrote the oversized tail");

    Ngs2RackInfo unchangedInfo;
    std::memset(&unchangedInfo, 0x6B, sizeof(unchangedInfo));
    const Ngs2RackInfo originalInfo = unchangedInfo;
    Require(sceNgs2RackGetInfo(rack, nullptr, sizeof(unchangedInfo)) != 0, "a null rack info output was accepted");
    Require(sceNgs2RackGetInfo(rack, &unchangedInfo, sizeof(unchangedInfo) - 1) != 0, "an undersized rack info was accepted");
    Require(std::memcmp(&unchangedInfo, &originalInfo, sizeof(unchangedInfo)) == 0, "an undersized rack info changed its output");
    Require(sceNgs2RackGetInfo(rack, &unchangedInfo, sizeof(oversizedInfo)) != 0, "an oversized rack info was accepted");
    Require(std::memcmp(&unchangedInfo, &originalInfo, sizeof(unchangedInfo)) == 0, "an oversized rack info changed its output");
    Require(sceNgs2RackGetInfo(0, &unchangedInfo, sizeof(unchangedInfo)) != 0, "an invalid rack info handle was accepted");
    Require(std::memcmp(&unchangedInfo, &originalInfo, sizeof(unchangedInfo)) == 0, "an invalid rack info handle changed its output");

    Ngs2Handle voice = 0;
    Require(sceNgs2RackGetVoiceHandle(rack, 3, &voice) == 0 && voice != 0, "the last voice of the rack is missing");
    Require(sceNgs2RackGetVoiceHandle(rack, 4, &voice) != 0, "a voice past the rack size was returned");
    Ngs2VoiceState state{};
    Require(sceNgs2VoiceGetState(voice, &state, sizeof(state)) == 0, "the voice state was refused");
    std::uint32_t flags = 0xFFFFFFFFu;
    Require(sceNgs2VoiceGetStateFlags(voice, &flags) == 0 && flags == 0, "a fresh voice is not idle");
    Require(sceNgs2VoiceGetStateFlags(0, &flags) != 0, "an invalid voice handle was accepted");

    Require(sceNgs2RackLock(rack) == 0 && sceNgs2RackUnlock(rack) == 0, "the rack cannot be locked");
    Require(sceNgs2RackLock(0) != 0, "an invalid rack handle was locked");

    Ngs2ContextBufferInfo returned{};
    Require(sceNgs2RackDestroy(rack, &returned) == 0, "the rack was not destroyed");
    Require(returned.host_buffer == rackStorage.data(), "the rack did not return its buffer");
    Require(sceNgs2RackDestroy(rack, &returned) != 0, "the rack was destroyed twice");
    Require(sceNgs2RackGetInfo(rack, &unchangedInfo, sizeof(unchangedInfo)) != 0, "a destroyed rack info handle was accepted");
    Require(std::memcmp(&unchangedInfo, &originalInfo, sizeof(unchangedInfo)) == 0, "a destroyed rack info handle changed its output");
    Require(sceNgs2VoiceGetStateFlags(voice, &flags) != 0, "a voice outlived its rack");

    alignas(8) std::array<std::uint8_t, 200> ps5OptionBytes{};
    WritePs5RackOption(ps5OptionBytes, 512, 5);
    const auto* ps5Option = reinterpret_cast<const Ngs2RackOption*>(ps5OptionBytes.data());
    Ngs2ContextBufferInfo ps5RackBuffer{};
    Require(sceNgs2RackQueryBufferSize(0, ps5Option, &ps5RackBuffer) == 0, "the PS5 rack option buffer size was refused");
    Require(ps5RackBuffer.host_buffer_size == 1024 + 5 * 512, "the PS5 rack option lost its voice count");
    std::vector<std::uint8_t> ps5RackStorage(ps5RackBuffer.host_buffer_size);
    ps5RackBuffer.host_buffer = ps5RackStorage.data();
    Ngs2Handle ps5Rack = 0;
    Require(sceNgs2RackCreate(system, 43, ps5Option, &ps5RackBuffer, &ps5Rack) == 0, "the PS5 rack option was not created");
    std::array<Ngs2Handle, 5> ps5Voices{};
    for (std::uint32_t index = 0; index < ps5Voices.size(); ++index) {
        Require(sceNgs2RackGetVoiceHandle(ps5Rack, index, &ps5Voices[index]) == 0 && ps5Voices[index] != 0,
                "a PS5 rack voice was not created");
        for (std::uint32_t previous = 0; previous < index; ++previous) {
            Require(ps5Voices[index] != ps5Voices[previous], "PS5 rack voices share a handle");
        }
    }
    Ngs2Handle ps5PastEnd = 0x1234;
    Require(sceNgs2RackGetVoiceHandle(ps5Rack, 5, &ps5PastEnd) != 0 && ps5PastEnd == 0x1234,
            "a voice past the PS5 rack size was returned");
    struct Ps5RackInfoWithCanary {
        Ngs2RackInfo info;
        std::uint8_t canary[64];
    } ps5Info;
    std::memset(&ps5Info, 0xA5, sizeof(ps5Info));
    Require(sceNgs2RackGetInfo(ps5Rack, &ps5Info.info, sizeof(ps5Info.info)) == 0, "the PS5 rack info was refused");
    Require(std::strcmp(ps5Info.info.name, "ps5-rack") == 0, "the PS5 rack name was not normalized");
    Require(ps5Info.info.max_grain_samples == 512 && ps5Info.info.max_voices == 5,
            "the PS5 rack limits were not normalized");
    bool ps5TailUnchanged = true;
    for (const auto value : ps5Info.canary) ps5TailUnchanged = ps5TailUnchanged && value == 0xA5;
    Require(ps5TailUnchanged, "the PS5 rack info overwrote its canary");

    Ngs2ContextBufferInfo unchangedBuffer{};
    std::memset(&unchangedBuffer, 0x6B, sizeof(unchangedBuffer));
    const Ngs2ContextBufferInfo originalBuffer = unchangedBuffer;
    Ngs2Handle unchangedRack = 0x5678;
    auto invalidPs5Option = ps5OptionBytes;
    const std::uint32_t invalidGrain = 2048;
    std::memcpy(invalidPs5Option.data() + 76, &invalidGrain, sizeof(invalidGrain));
    Require(sceNgs2RackQueryBufferSize(0, reinterpret_cast<const Ngs2RackOption*>(invalidPs5Option.data()), &unchangedBuffer) != 0,
            "an invalid PS5 rack grain was accepted");
    Require(std::memcmp(&unchangedBuffer, &originalBuffer, sizeof(unchangedBuffer)) == 0,
            "an invalid PS5 rack grain changed its query output");
    Require(sceNgs2RackCreate(system, 43, reinterpret_cast<const Ngs2RackOption*>(invalidPs5Option.data()),
                              &ps5RackBuffer, &unchangedRack) != 0 && unchangedRack == 0x5678,
            "an invalid PS5 rack grain changed the create output");
    invalidPs5Option = ps5OptionBytes;
    const std::uint32_t invalidVoices = 513;
    std::memcpy(invalidPs5Option.data() + 80, &invalidVoices, sizeof(invalidVoices));
    Require(sceNgs2RackQueryBufferSize(0, reinterpret_cast<const Ngs2RackOption*>(invalidPs5Option.data()), &unchangedBuffer) != 0,
            "an invalid PS5 rack voice count was accepted");
    Require(std::memcmp(&unchangedBuffer, &originalBuffer, sizeof(unchangedBuffer)) == 0,
            "an invalid PS5 rack voice count changed its query output");
    Require(sceNgs2RackCreate(system, 43, reinterpret_cast<const Ngs2RackOption*>(invalidPs5Option.data()),
                              &ps5RackBuffer, &unchangedRack) != 0 && unchangedRack == 0x5678,
            "an invalid PS5 rack voice count changed the create output");
    invalidPs5Option = ps5OptionBytes;
    const std::size_t unsupportedSize = 192;
    std::memcpy(invalidPs5Option.data(), &unsupportedSize, sizeof(unsupportedSize));
    Require(sceNgs2RackQueryBufferSize(0, reinterpret_cast<const Ngs2RackOption*>(invalidPs5Option.data()), &unchangedBuffer) != 0,
            "an unsupported PS5 rack option size was accepted");
    Require(std::memcmp(&unchangedBuffer, &originalBuffer, sizeof(unchangedBuffer)) == 0,
            "an unsupported PS5 rack option size changed its query output");
    Require(sceNgs2RackCreate(system, 43, reinterpret_cast<const Ngs2RackOption*>(invalidPs5Option.data()),
                              &ps5RackBuffer, &unchangedRack) != 0 && unchangedRack == 0x5678,
            "an unsupported PS5 rack option size changed the create output");
    Require(sceNgs2RackDestroy(ps5Rack, nullptr) == 0, "the PS5 rack was not destroyed");

    const Ngs2BufferAllocator allocator{Allocate, Release, 0};
    Ngs2Handle allocated = 0;
    Require(sceNgs2SystemCreateWithAllocator(&systemOption, &allocator, &allocated) == 0, "the allocator path failed");
    Require(allocations == 1, "the allocator was not called");
    Require(sceNgs2SystemDestroy(allocated, nullptr) == 0, "the allocated system was not destroyed");
    Require(releases == 1, "the allocated buffer was not released");

    Ngs2Handle allocatedPs5Rack = 0;
    Require(sceNgs2RackCreateWithAllocator(system, 44, ps5Option, &allocator, &allocatedPs5Rack) == 0,
            "the PS5 rack allocator path failed");
    Ngs2Handle allocatedPs5Voice = 0;
    Require(sceNgs2RackGetVoiceHandle(allocatedPs5Rack, 4, &allocatedPs5Voice) == 0 && allocatedPs5Voice != 0,
            "the PS5 rack allocator lost its voice count");
    Require(allocations == 2, "the PS5 rack allocator was not called");
    Require(sceNgs2RackDestroy(allocatedPs5Rack, nullptr) == 0, "the allocated PS5 rack was not destroyed");
    Require(releases == 2, "the allocated PS5 rack buffer was not released");

    Require(sceNgs2SystemDestroy(system, &returned) == 0, "the system was not destroyed");
    Require(returned.host_buffer == storage.data(), "the system did not return its buffer");
    Require(sceNgs2SystemDestroy(system, &returned) != 0, "the system was destroyed twice");

    if (failures != 0) return 1;
    std::printf("Ngs2 lifecycle tests passed\n");
    return 0;
}
