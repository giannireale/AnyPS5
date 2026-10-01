#include <cstdint>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "SceTypes.hpp"

namespace {

constexpr int SCE_NGS2_ERROR_FAIL = static_cast<int>(0x804A0001);
constexpr int SCE_NGS2_ERROR_INVALID_MAX_GRAIN_SAMPLES = static_cast<int>(0x804A0050);
constexpr int SCE_NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES = static_cast<int>(0x804A0051);
constexpr int SCE_NGS2_ERROR_INVALID_OUT_ADDRESS = static_cast<int>(0x804A0053);
constexpr int SCE_NGS2_ERROR_INVALID_OUT_SIZE = static_cast<int>(0x804A0054);
constexpr int SCE_NGS2_ERROR_INVALID_OPTION_ADDRESS = static_cast<int>(0x804A0080);
constexpr int SCE_NGS2_ERROR_INVALID_OPTION_SIZE = static_cast<int>(0x804A0081);
constexpr int SCE_NGS2_ERROR_INVALID_MAX_VOICES = static_cast<int>(0x804A0103);
constexpr int SCE_NGS2_ERROR_INVALID_BUFFER_ADDRESS = static_cast<int>(0x804A0180);
constexpr int SCE_NGS2_ERROR_INVALID_BUFFER_SIZE = static_cast<int>(0x804A0181);
constexpr int SCE_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR = static_cast<int>(0x804A0182);
constexpr int SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE = static_cast<int>(0x804A0200);
constexpr int SCE_NGS2_ERROR_INVALID_RACK_HANDLE = static_cast<int>(0x804A0201);
constexpr int SCE_NGS2_ERROR_INVALID_VOICE_HANDLE = static_cast<int>(0x804A0202);
constexpr int SCE_NGS2_ERROR_INVALID_RACK_ID = static_cast<int>(0x804A0210);
constexpr int SCE_NGS2_ERROR_INVALID_VOICE_ID = static_cast<int>(0x804A0211);
// Taken from the PS4 NGS2 error table (shadPS4 ngs2_error.h); the PS5 value is unconfirmed.
constexpr int SCE_NGS2_ERROR_INVALID_VOICE_PORT_INDEX = static_cast<int>(0x804A0304);

// sceNgs2VoiceGetPortInfo output. The title passes a 24-byte buffer (RCX=0x18 at the call),
// which matches the PS4 layout: matrix id, volume, delay samples, destination input, voice.
struct Ngs2VoicePortInfo {
    std::int32_t matrix_id;
    float volume;
    std::uint32_t num_delay_samples;
    std::uint32_t dest_input_id;
    Ngs2Handle dest_voice_handle;
};
static_assert(sizeof(Ngs2VoicePortInfo) == 24 && offsetof(Ngs2VoicePortInfo, dest_voice_handle) == 16);

constexpr std::uint32_t MinGrainSamples = 64;
constexpr std::uint32_t MaxGrainSamples = 1024;
constexpr std::uint32_t DefaultGrainSamples = 256;
constexpr std::uint32_t DefaultSampleRate = 48000;
constexpr std::uint32_t MaxRackVoices = 512;
constexpr std::uint32_t SystemStateReady = 1;
constexpr std::size_t SystemBufferBase = 4096;
constexpr std::size_t RackBufferBase = 1024;
constexpr std::size_t RackBufferPerVoice = 512;
constexpr std::size_t GrainBufferScale = 16;

struct Rack;

struct Voice {
    Ngs2Handle handle = 0;
    std::uint32_t id = 0;
    std::uint32_t stateFlags = 0;
    Rack* rack = nullptr;
};

struct System;

struct Rack {
    Ngs2Handle handle = 0;
    std::uint32_t rackId = 0;
    std::uint32_t uid = 0;
    Ngs2RackOption option{};
    Ngs2ContextBufferInfo bufferInfo{};
    bool ownsBuffer = false;
    Ngs2BufferAllocator allocator{};
    bool locked = false;
    System* system = nullptr;
    std::vector<std::unique_ptr<Voice>> voices;
};

struct System {
    Ngs2Handle handle = 0;
    Ngs2SystemOption option{};
    Ngs2ContextBufferInfo bufferInfo{};
    bool ownsBuffer = false;
    Ngs2BufferAllocator allocator{};
    std::uint32_t uid = 0;
    std::uint32_t grainSamples = DefaultGrainSamples;
    std::uint32_t sampleRate = DefaultSampleRate;
    std::int64_t renderCount = 0;
    std::vector<std::unique_ptr<Rack>> racks;
};

std::mutex g_lock;
std::map<Ngs2Handle, std::unique_ptr<System>> g_systems;
std::map<Ngs2Handle, Rack*> g_racks;
std::map<Ngs2Handle, Voice*> g_voices;
Ngs2Handle g_nextHandle = 0x4E470001;
std::uint32_t g_nextUid = 1;

Ngs2Handle nextHandle() {
    return g_nextHandle += 4;
}

System* findSystem(const Ngs2Handle handle) {
    const auto entry = g_systems.find(handle);
    return entry == g_systems.end() ? nullptr : entry->second.get();
}

Rack* findRack(const Ngs2Handle handle) {
    const auto entry = g_racks.find(handle);
    return entry == g_racks.end() ? nullptr : entry->second;
}

Voice* findVoice(const Ngs2Handle handle) {
    const auto entry = g_voices.find(handle);
    return entry == g_voices.end() ? nullptr : entry->second;
}

int validateSystemOption(const Ngs2SystemOption* option, std::uint32_t* outGrain, std::uint32_t* outRate) {
    std::uint32_t grain = DefaultGrainSamples;
    std::uint32_t rate = DefaultSampleRate;
    if (option != nullptr) {
        if (option->size != sizeof(Ngs2SystemOption)) return SCE_NGS2_ERROR_INVALID_OPTION_SIZE;
        if (option->max_grain_samples != 0) {
            if (option->max_grain_samples < MinGrainSamples || option->max_grain_samples > MaxGrainSamples)
                return SCE_NGS2_ERROR_INVALID_MAX_GRAIN_SAMPLES;
            grain = option->max_grain_samples;
        }
        if (option->num_grain_samples != 0) {
            if (option->num_grain_samples < MinGrainSamples || option->num_grain_samples > grain)
                return SCE_NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES;
            grain = option->num_grain_samples;
        }
        if (option->sample_rate != 0) rate = option->sample_rate;
    }
    *outGrain = grain;
    *outRate = rate;
    return 0;
}

constexpr std::size_t Ps5RackOptionSize = 200;
constexpr std::size_t Ps5MasteringRackOptionSize = 184;
constexpr std::size_t Ps5RackOptionFlagsOffset = 72;
constexpr std::size_t Ps5RackOptionMaxGrainOffset = 76;
constexpr std::size_t Ps5RackOptionMaxVoicesOffset = 80;

int normalizeRackOption(const Ngs2RackOption* option, Ngs2RackOption* normalized, std::uint32_t* outVoices) {
    std::uint32_t voices = 1;
    Ngs2RackOption value{};
    if (option != nullptr) {
        std::size_t optionSize = 0;
        std::memcpy(&optionSize, option, sizeof(optionSize));
        if (optionSize == sizeof(Ngs2RackOption)) {
            value = *option;
        } else if (optionSize == Ps5RackOptionSize || optionSize == Ps5MasteringRackOptionSize) {
            // The observed SDK layouts share these fields; their remaining bytes are opaque.
            value.size = sizeof(Ngs2RackOption);
            std::memcpy(value.name, reinterpret_cast<const std::uint8_t*>(option) + 8, sizeof(value.name));
            std::memcpy(&value.flags, reinterpret_cast<const std::uint8_t*>(option) + Ps5RackOptionFlagsOffset, sizeof(value.flags));
            std::memcpy(&value.max_grain_samples, reinterpret_cast<const std::uint8_t*>(option) + Ps5RackOptionMaxGrainOffset, sizeof(value.max_grain_samples));
            std::memcpy(&value.max_voices, reinterpret_cast<const std::uint8_t*>(option) + Ps5RackOptionMaxVoicesOffset, sizeof(value.max_voices));
        } else {
            return SCE_NGS2_ERROR_INVALID_OPTION_SIZE;
        }
        if (value.max_voices > MaxRackVoices) return SCE_NGS2_ERROR_INVALID_MAX_VOICES;
        if (value.max_voices != 0) voices = value.max_voices;
        if (value.max_grain_samples > MaxGrainSamples) return SCE_NGS2_ERROR_INVALID_MAX_GRAIN_SAMPLES;
    }
    if (normalized != nullptr) *normalized = value;
    *outVoices = voices;
    return 0;
}

std::size_t systemBufferSize(const std::uint32_t grainSamples) {
    return SystemBufferBase + static_cast<std::size_t>(grainSamples) * GrainBufferScale;
}

std::size_t rackBufferSize(const std::uint32_t voices) {
    return RackBufferBase + static_cast<std::size_t>(voices) * RackBufferPerVoice;
}

int acquireBuffer(const Ngs2BufferAllocator* allocator, const std::size_t size, Ngs2ContextBufferInfo* out) {
    if (allocator == nullptr || allocator->alloc_handler == nullptr) return SCE_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR;
    Ngs2ContextBufferInfo info{};
    info.host_buffer_size = size;
    info.user_data = allocator->user_data;
    if (allocator->alloc_handler(&info) != 0 || info.host_buffer == nullptr) return SCE_NGS2_ERROR_FAIL;
    if (info.host_buffer_size < size) return SCE_NGS2_ERROR_INVALID_BUFFER_SIZE;
    *out = info;
    return 0;
}

void releaseBuffer(const Ngs2BufferAllocator& allocator, Ngs2ContextBufferInfo& info) {
    if (allocator.free_handler != nullptr) allocator.free_handler(&info);
}

int createSystem(const Ngs2SystemOption* option, const Ngs2ContextBufferInfo& bufferInfo, const bool ownsBuffer,
                 const Ngs2BufferAllocator& allocator, Ngs2Handle* handle) {
    std::uint32_t grain = 0;
    std::uint32_t rate = 0;
    const int optionResult = validateSystemOption(option, &grain, &rate);
    if (optionResult != 0) return optionResult;
    if (bufferInfo.host_buffer == nullptr) return SCE_NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    if (bufferInfo.host_buffer_size < systemBufferSize(grain)) return SCE_NGS2_ERROR_INVALID_BUFFER_SIZE;

    auto created = std::make_unique<System>();
    created->handle = nextHandle();
    if (option != nullptr) created->option = *option;
    created->bufferInfo = bufferInfo;
    created->ownsBuffer = ownsBuffer;
    created->allocator = allocator;
    created->uid = g_nextUid++;
    created->grainSamples = grain;
    created->sampleRate = rate;
    *handle = created->handle;
    g_systems[created->handle] = std::move(created);
    return 0;
}

int createRack(System* system, const std::uint32_t rackId, const Ngs2RackOption* option,
               const Ngs2ContextBufferInfo& bufferInfo, const bool ownsBuffer,
               const Ngs2BufferAllocator& allocator, Ngs2Handle* handle) {
    std::uint32_t voices = 0;
    Ngs2RackOption normalizedOption{};
    const int optionResult = normalizeRackOption(option, &normalizedOption, &voices);
    if (optionResult != 0) return optionResult;
    if (bufferInfo.host_buffer == nullptr) return SCE_NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    if (bufferInfo.host_buffer_size < rackBufferSize(voices)) return SCE_NGS2_ERROR_INVALID_BUFFER_SIZE;

    auto created = std::make_unique<Rack>();
    created->handle = nextHandle();
    created->rackId = rackId;
    created->uid = g_nextUid++;
    created->option = normalizedOption;
    created->bufferInfo = bufferInfo;
    created->ownsBuffer = ownsBuffer;
    created->allocator = allocator;
    created->system = system;
    for (std::uint32_t index = 0; index < voices; ++index) {
        auto voice = std::make_unique<Voice>();
        voice->handle = nextHandle();
        voice->id = index;
        voice->rack = created.get();
        g_voices[voice->handle] = voice.get();
        created->voices.push_back(std::move(voice));
    }
    *handle = created->handle;
    g_racks[created->handle] = created.get();
    system->racks.push_back(std::move(created));
    return 0;
}

void destroyRack(System* system, Rack* rack, Ngs2ContextBufferInfo* bufferInfo) {
    for (const auto& voice : rack->voices) g_voices.erase(voice->handle);
    g_racks.erase(rack->handle);
    if (bufferInfo != nullptr) *bufferInfo = rack->bufferInfo;
    if (rack->ownsBuffer) releaseBuffer(rack->allocator, rack->bufferInfo);
    for (auto entry = system->racks.begin(); entry != system->racks.end(); ++entry) {
        if (entry->get() == rack) {
            system->racks.erase(entry);
            return;
        }
    }
}

}

extern "C" {

int APS5_VABI sceNgs2SystemQueryBufferSize(const Ngs2SystemOption* option, Ngs2ContextBufferInfo* buffer_info) {
    if (buffer_info == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::uint32_t grain = 0;
    std::uint32_t rate = 0;
    const int optionResult = validateSystemOption(option, &grain, &rate);
    if (optionResult != 0) return optionResult;
    buffer_info->host_buffer = nullptr;
    buffer_info->host_buffer_size = systemBufferSize(grain);
    return 0;
}

int APS5_VABI sceNgs2SystemCreate(const Ngs2SystemOption* option, const Ngs2ContextBufferInfo* buffer_info, Ngs2Handle* handle) {
    if (handle == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (buffer_info == nullptr) return SCE_NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    std::lock_guard lock(g_lock);
    return createSystem(option, *buffer_info, false, Ngs2BufferAllocator{}, handle);
}

int APS5_VABI sceNgs2SystemCreateWithAllocator(const Ngs2SystemOption* option, const Ngs2BufferAllocator* allocator, Ngs2Handle* handle) {
    if (handle == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::uint32_t grain = 0;
    std::uint32_t rate = 0;
    const int optionResult = validateSystemOption(option, &grain, &rate);
    if (optionResult != 0) return optionResult;
    Ngs2ContextBufferInfo info{};
    const int bufferResult = acquireBuffer(allocator, systemBufferSize(grain), &info);
    if (bufferResult != 0) return bufferResult;
    std::lock_guard lock(g_lock);
    const int result = createSystem(option, info, true, *allocator, handle);
    if (result != 0) releaseBuffer(*allocator, info);
    return result;
}

int APS5_VABI sceNgs2SystemDestroy(Ngs2Handle system_handle, Ngs2ContextBufferInfo* buffer_info) {
    std::lock_guard lock(g_lock);
    System* system = findSystem(system_handle);
    if (system == nullptr) return SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    while (!system->racks.empty()) destroyRack(system, system->racks.back().get(), nullptr);
    if (buffer_info != nullptr) *buffer_info = system->bufferInfo;
    if (system->ownsBuffer) releaseBuffer(system->allocator, system->bufferInfo);
    g_systems.erase(system_handle);
    return 0;
}

int APS5_VABI sceNgs2SystemGetInfo(Ngs2Handle system_handle, Ngs2SystemInfo* info, std::size_t info_size) {
    if (info == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (info_size < sizeof(Ngs2SystemInfo)) return SCE_NGS2_ERROR_INVALID_OUT_SIZE;
    std::lock_guard lock(g_lock);
    const System* system = findSystem(system_handle);
    if (system == nullptr) return SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    std::memset(info, 0, sizeof(Ngs2SystemInfo));
    std::memcpy(info->name, system->option.name, sizeof(info->name));
    info->system_handle = system->handle;
    info->buffer_info = system->bufferInfo;
    info->uid = system->uid;
    info->min_grain_samples = MinGrainSamples;
    info->max_grain_samples = MaxGrainSamples;
    info->state_flags = SystemStateReady;
    info->rack_count = static_cast<std::uint32_t>(system->racks.size());
    info->render_count = system->renderCount;
    info->sample_rate = system->sampleRate;
    info->num_grain_samples = system->grainSamples;
    return 0;
}

int APS5_VABI sceNgs2SystemSetGrainSamples(Ngs2Handle system_handle, std::uint32_t num_samples) {
    if (num_samples < MinGrainSamples || num_samples > MaxGrainSamples) return SCE_NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES;
    std::lock_guard lock(g_lock);
    System* system = findSystem(system_handle);
    if (system == nullptr) return SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    system->grainSamples = num_samples;
    return 0;
}

int APS5_VABI sceNgs2SystemRender(Ngs2Handle system_handle, const Ngs2RenderBufferInfo* buffer_info,
                                 std::uint32_t num_buffer_info) {
    (void)buffer_info;
    (void)num_buffer_info;
    std::lock_guard lock(g_lock);
    if (findSystem(system_handle) == nullptr) return SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    // The mixer and render-buffer formats are not implemented.
    return SCE_NGS2_ERROR_FAIL;
}

int APS5_VABI sceNgs2RackQueryBufferSize(std::uint32_t rack_id, const Ngs2RackOption* option, Ngs2ContextBufferInfo* buffer_info) {
    (void)rack_id;
    if (buffer_info == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::uint32_t voices = 0;
    const int optionResult = normalizeRackOption(option, nullptr, &voices);
    if (optionResult != 0) return optionResult;
    buffer_info->host_buffer = nullptr;
    buffer_info->host_buffer_size = rackBufferSize(voices);
    return 0;
}

int APS5_VABI sceNgs2RackCreate(Ngs2Handle system_handle, std::uint32_t rack_id, const Ngs2RackOption* option,
                                const Ngs2ContextBufferInfo* buffer_info, Ngs2Handle* handle) {
    if (handle == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (buffer_info == nullptr) return SCE_NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    std::lock_guard lock(g_lock);
    System* system = findSystem(system_handle);
    if (system == nullptr) return SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    return createRack(system, rack_id, option, *buffer_info, false, Ngs2BufferAllocator{}, handle);
}

int APS5_VABI sceNgs2RackCreateWithAllocator(Ngs2Handle system_handle, std::uint32_t rack_id, const Ngs2RackOption* option,
                                             const Ngs2BufferAllocator* allocator, Ngs2Handle* handle) {
    if (handle == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::uint32_t voices = 0;
    const int optionResult = normalizeRackOption(option, nullptr, &voices);
    if (optionResult != 0) return optionResult;
    Ngs2ContextBufferInfo info{};
    const int bufferResult = acquireBuffer(allocator, rackBufferSize(voices), &info);
    if (bufferResult != 0) return bufferResult;
    std::lock_guard lock(g_lock);
    System* system = findSystem(system_handle);
    if (system == nullptr) {
        releaseBuffer(*allocator, info);
        return SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    const int result = createRack(system, rack_id, option, info, true, *allocator, handle);
    if (result != 0) releaseBuffer(*allocator, info);
    return result;
}

int APS5_VABI sceNgs2RackDestroy(Ngs2Handle rack_handle, Ngs2ContextBufferInfo* buffer_info) {
    std::lock_guard lock(g_lock);
    Rack* rack = findRack(rack_handle);
    if (rack == nullptr) return SCE_NGS2_ERROR_INVALID_RACK_HANDLE;
    destroyRack(rack->system, rack, buffer_info);
    return 0;
}

int APS5_VABI sceNgs2RackGetInfo(Ngs2Handle rack_handle, Ngs2RackInfo* info, std::size_t info_size) {
    if (info == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (info_size != sizeof(Ngs2RackInfo)) return SCE_NGS2_ERROR_INVALID_OUT_SIZE;
    std::lock_guard lock(g_lock);
    const Rack* rack = findRack(rack_handle);
    if (rack == nullptr) return SCE_NGS2_ERROR_INVALID_RACK_HANDLE;
    std::memset(info, 0, sizeof(Ngs2RackInfo));
    std::memcpy(info->name, rack->option.name, sizeof(info->name));
    info->rack_handle = rack->handle;
    info->buffer_info = rack->bufferInfo;
    info->owner_system_handle = rack->system->handle;
    info->rack_id = rack->rackId;
    info->uid = rack->uid;
    info->min_grain_samples = MinGrainSamples;
    info->max_grain_samples = rack->option.max_grain_samples != 0
                                  ? rack->option.max_grain_samples
                                  : (rack->system->option.max_grain_samples != 0
                                         ? rack->system->option.max_grain_samples
                                         : MaxGrainSamples);
    info->max_voices = static_cast<std::uint32_t>(rack->voices.size());
    info->max_matrices = rack->option.max_matrices;
    info->max_ports = rack->option.max_ports;
    return 0;
}

int APS5_VABI sceNgs2RackGetVoiceHandle(Ngs2Handle rack_handle, std::uint32_t voice_id, Ngs2Handle* handle) {
    if (handle == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::lock_guard lock(g_lock);
    const Rack* rack = findRack(rack_handle);
    if (rack == nullptr) return SCE_NGS2_ERROR_INVALID_RACK_HANDLE;
    if (voice_id >= rack->voices.size()) return SCE_NGS2_ERROR_INVALID_VOICE_ID;
    *handle = rack->voices[voice_id]->handle;
    return 0;
}

int APS5_VABI sceNgs2RackLock(Ngs2Handle rack_handle) {
    std::lock_guard lock(g_lock);
    Rack* rack = findRack(rack_handle);
    if (rack == nullptr) return SCE_NGS2_ERROR_INVALID_RACK_HANDLE;
    rack->locked = true;
    return 0;
}

int APS5_VABI sceNgs2RackUnlock(Ngs2Handle rack_handle) {
    std::lock_guard lock(g_lock);
    Rack* rack = findRack(rack_handle);
    if (rack == nullptr) return SCE_NGS2_ERROR_INVALID_RACK_HANDLE;
    rack->locked = false;
    return 0;
}

int APS5_VABI sceNgs2VoiceControl(Ngs2Handle voice_handle, const Ngs2VoiceParamHeader* param_list) {
    std::lock_guard lock(g_lock);
    if (findVoice(voice_handle) == nullptr) return SCE_NGS2_ERROR_INVALID_VOICE_HANDLE;
    // PS5 parameter blocks differ from Ngs2VoiceParamHeader. Until their
    // semantics are implemented, reject them without reading or applying them.
    (void)param_list;
    return SCE_NGS2_ERROR_FAIL;
}

int APS5_VABI sceNgs2VoiceRunCommands(Ngs2Handle voice_handle, const void* commands, std::uint32_t num_commands) {
    std::lock_guard lock(g_lock);
    if (findVoice(voice_handle) == nullptr) return SCE_NGS2_ERROR_INVALID_VOICE_HANDLE;
    if (num_commands == 0) return 0;
    // Observed SDK commands are not VoiceControl parameter blocks. Never
    // reinterpret them as such or report that an unsupported batch ran.
    (void)commands;
    return SCE_NGS2_ERROR_FAIL;
}

int APS5_VABI sceNgs2VoiceGetState(Ngs2Handle voice_handle, Ngs2VoiceState* state, std::size_t state_size) {
    if (state == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (state_size < sizeof(Ngs2VoiceState)) return SCE_NGS2_ERROR_INVALID_OUT_SIZE;
    std::lock_guard lock(g_lock);
    const Voice* voice = findVoice(voice_handle);
    if (voice == nullptr) return SCE_NGS2_ERROR_INVALID_VOICE_HANDLE;
    state->state_flags = voice->stateFlags;
    return 0;
}

int APS5_VABI sceNgs2VoiceGetStateFlags(Ngs2Handle voice_handle, std::uint32_t* state_flags) {
    if (state_flags == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::lock_guard lock(g_lock);
    const Voice* voice = findVoice(voice_handle);
    if (voice == nullptr) return SCE_NGS2_ERROR_INVALID_VOICE_HANDLE;
    *state_flags = voice->stateFlags;
    return 0;
}

// No voice patching exists yet, so every port of a valid voice is unconnected: no matrix, no
// destination. A rack without an explicit port count has one port per voice.
int APS5_VABI sceNgs2VoiceGetPortInfo(Ngs2Handle voice_handle, std::uint32_t port, Ngs2VoicePortInfo* info, std::size_t info_size) {
    std::lock_guard lock(g_lock);
    const Voice* voice = findVoice(voice_handle);
    if (voice == nullptr) return SCE_NGS2_ERROR_INVALID_VOICE_HANDLE;
    if (info == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (info_size < sizeof(Ngs2VoicePortInfo)) return SCE_NGS2_ERROR_INVALID_OUT_SIZE;
    const std::uint32_t ports = voice->rack->option.max_ports != 0 ? voice->rack->option.max_ports : 1;
    if (port >= ports) return SCE_NGS2_ERROR_INVALID_VOICE_PORT_INDEX;
    *info = Ngs2VoicePortInfo{-1, 0.0f, 0, 0, 0};
    return 0;
}

}
