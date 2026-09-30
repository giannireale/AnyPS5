#include "SceTypes.hpp"

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
int APS5_VABI sceNgs2RackDestroy(Ngs2Handle rack_handle, Ngs2ContextBufferInfo* buffer_info);
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
    option.max_voices = voices;
    return option;
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
    Require(sceNgs2RackCreate(system, 0, &rackOption, &rackBuffer, &rack) == 0, "the rack was not created");
    Require(sceNgs2RackCreate(0, 0, &rackOption, &rackBuffer, &rack) != 0, "a rack on an invalid system was accepted");
    Require(sceNgs2SystemGetInfo(system, &info, sizeof(info)) == 0 && info.rack_count == 1, "the system does not count its rack");

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
    Require(sceNgs2VoiceGetStateFlags(voice, &flags) != 0, "a voice outlived its rack");

    const Ngs2BufferAllocator allocator{Allocate, Release, 0};
    Ngs2Handle allocated = 0;
    Require(sceNgs2SystemCreateWithAllocator(&systemOption, &allocator, &allocated) == 0, "the allocator path failed");
    Require(allocations == 1, "the allocator was not called");
    Require(sceNgs2SystemDestroy(allocated, nullptr) == 0, "the allocated system was not destroyed");
    Require(releases == 1, "the allocated buffer was not released");

    Require(sceNgs2SystemDestroy(system, &returned) == 0, "the system was not destroyed");
    Require(returned.host_buffer == storage.data(), "the system did not return its buffer");
    Require(sceNgs2SystemDestroy(system, &returned) != 0, "the system was destroyed twice");

    if (failures != 0) return 1;
    std::printf("Ngs2 lifecycle tests passed\n");
    return 0;
}
