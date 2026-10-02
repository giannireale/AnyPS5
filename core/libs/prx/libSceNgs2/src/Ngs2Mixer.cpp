#include "Ngs2Mixer.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace ngs2mix {
namespace {

constexpr std::uint32_t WaveformPcmS16 = 0x12;
constexpr std::uint32_t StateInUse = 0x1;
constexpr std::uint32_t StatePlaying = 0x2;
constexpr std::uint32_t InfiniteRepeats = 0xFFFFFFFF;
constexpr std::uint32_t MaxBlocks = 256;
constexpr std::uint32_t MaxRenderChannels = 8;

// Generic and rack-specific parameter ids observed in ANIMAL WELL's wrappers (eboot 0x17xxx-0x18xxx).
constexpr std::uint32_t ParamPatch = 0x00000005;
constexpr std::uint32_t ParamSamplerSetup = 0x10000000;
constexpr std::uint32_t ParamSamplerBlocks = 0x10000001;
constexpr std::uint32_t ParamSubmixerSetup = 0x20000000;
constexpr std::uint32_t ParamMasteringSetup = 0x30000000;
constexpr std::uint32_t ParamMasteringOutput = 0x30000005;

// Command records are 32 bytes: word 0 low byte is the type. The title's play and stop
// helpers (eboot 0x177e0 / 0x178c0) send type 2 with event 1 and 2; volume is type 6.
constexpr std::uint32_t CommandEvent = 2;
constexpr std::uint32_t CommandVolume = 6;
constexpr std::uint32_t EventPlay = 1;
constexpr std::uint32_t EventStop = 2;

template <class T>
T load(const std::uint8_t* source) {
    T value{};
    std::memcpy(&value, source, sizeof(value));
    return value;
}

void rewind(VoiceMix& voice) {
    voice.blockIndex = 0;
    voice.repeatsDone = 0;
    voice.position = voice.blocks.empty() ? 0.0 : voice.blocks.front().numSkipSamples;
}

// Moves past the finished block (or repeats it); stops the voice after the last one.
void advanceBlock(VoiceMix& voice) {
    const WaveformBlock& block = voice.blocks[voice.blockIndex];
    const bool playable = block.numSamples > block.numSkipSamples;
    if (playable && (block.numRepeats == InfiniteRepeats || voice.repeatsDone < block.numRepeats)) {
        ++voice.repeatsDone;
        voice.position -= block.numSamples - block.numSkipSamples;
        return;
    }
    ++voice.blockIndex;
    voice.repeatsDone = 0;
    if (voice.blockIndex >= voice.blocks.size()) {
        voice.playing = false;
        return;
    }
    voice.position = voice.blocks[voice.blockIndex].numSkipSamples;
}

float sampleAt(const VoiceMix& voice, const WaveformBlock& block, const std::uint32_t frame, const std::uint32_t channel) {
    const std::uint64_t byte = (static_cast<std::uint64_t>(frame) * voice.channels + channel) * sizeof(std::int16_t);
    if (byte + sizeof(std::int16_t) > block.dataSize) return 0.0f;
    return static_cast<float>(load<std::int16_t>(voice.data + block.dataOffset + byte)) / 32768.0f;
}

void renderSampler(VoiceMix& voice, const std::uint32_t grain, const std::uint32_t systemRate) {
    if (!voice.playing || voice.data == nullptr || voice.waveformType != WaveformPcmS16) return;
    if (voice.channels == 0 || voice.channels > 2 || voice.sampleRate == 0 || systemRate == 0) return;
    if (voice.blocks.empty()) {
        voice.playing = false;
        return;
    }
    const double step = static_cast<double>(voice.sampleRate) / systemRate;
    for (std::uint32_t index = 0; index < grain; ++index) {
        while (voice.playing && voice.position >= voice.blocks[voice.blockIndex].numSamples) advanceBlock(voice);
        if (!voice.playing) return;
        const WaveformBlock& block = voice.blocks[voice.blockIndex];
        const auto frame = static_cast<std::uint32_t>(voice.position);
        const float left = sampleAt(voice, block, frame, 0);
        const float right = voice.channels == 2 ? sampleAt(voice, block, frame, 1) : left;
        voice.output[index * 2] = left * voice.volume;
        voice.output[index * 2 + 1] = right * voice.volume;
        voice.position += step;
    }
}

void writeMastering(const VoiceMix& voice, const Ngs2RenderBufferInfo* buffers, const std::uint32_t numBuffers, const std::uint32_t grain) {
    if (voice.renderBufferIndex < 0 || static_cast<std::uint32_t>(voice.renderBufferIndex) >= numBuffers) return;
    const Ngs2RenderBufferInfo& buffer = buffers[voice.renderBufferIndex];
    if (buffer.waveform_type != WaveformPcmS16) return;
    auto* samples = static_cast<std::int16_t*>(buffer.buffer);
    for (std::uint32_t index = 0; index < grain; ++index) {
        const float left = voice.output[index * 2];
        const float right = voice.output[index * 2 + 1];
        for (std::uint32_t channel = 0; channel < buffer.num_channels; ++channel) {
            const float value = buffer.num_channels == 1 ? (left + right) * 0.5f : channel == 0 ? left : channel == 1 ? right : 0.0f;
            std::int16_t& target = samples[index * buffer.num_channels + channel];
            const float mixed = static_cast<float>(target) + value * 32767.0f;
            target = static_cast<std::int16_t>(std::clamp(mixed, -32768.0f, 32767.0f));
        }
    }
}

} // namespace

bool ApplyParam(VoiceMix& voice, const std::uint32_t id, const std::uint8_t* payload, const std::size_t size) {
    if (id == ParamPatch) {
        if (size < sizeof(Patch)) return false;
        const Patch patch = load<Patch>(payload);
        std::erase_if(voice.patches, [&patch](const Patch& existing) { return existing.port == patch.port; });
        if (patch.destHandle != 0) voice.patches.push_back(patch);
        return true;
    }
    if ((id >> 16) != voice.rackId) return false;
    switch (id) {
    case ParamSamplerSetup:
        if (size < 3 * sizeof(std::uint32_t)) return false;
        voice.waveformType = load<std::uint32_t>(payload);
        voice.channels = load<std::uint32_t>(payload + 4);
        voice.sampleRate = load<std::uint32_t>(payload + 8);
        return true;
    case ParamSamplerBlocks: {
        if (size < 0x18) return false;
        const auto data = load<std::uint64_t>(payload);
        const auto numBlocks = load<std::uint32_t>(payload + 0xC);
        const auto blocks = load<std::uint64_t>(payload + 0x10);
        if (numBlocks > MaxBlocks || (numBlocks != 0 && (data == 0 || blocks == 0))) return false;
        voice.data = reinterpret_cast<const std::uint8_t*>(data);
        voice.blocks.resize(numBlocks);
        if (numBlocks != 0) std::memcpy(voice.blocks.data(), reinterpret_cast<const void*>(blocks), numBlocks * sizeof(WaveformBlock));
        rewind(voice);
        return true;
    }
    case ParamSubmixerSetup:
    case ParamMasteringSetup:
        // Channel count of the bus; the mix runs in stereo and both titles' buses are stereo.
        return size >= sizeof(std::uint32_t) && load<std::uint32_t>(payload) == 2;
    case ParamMasteringOutput:
        if (size < sizeof(std::int32_t)) return false;
        voice.renderBufferIndex = load<std::int32_t>(payload);
        return true;
    default:
        return false;
    }
}

bool RunCommand(VoiceMix& voice, const std::uint32_t* record) {
    switch (record[0] & 0xFF) {
    case CommandEvent:
        if (record[2] == EventPlay) {
            voice.playing = voice.rackId != RackSampler || !voice.blocks.empty();
            rewind(voice);
            return true;
        }
        if (record[2] == EventStop) {
            voice.playing = false;
            return true;
        }
        return false;
    case CommandVolume: {
        float volume = 0.0f;
        std::memcpy(&volume, &record[2], sizeof(volume));
        if (!(volume >= 0.0f && volume <= 16.0f)) return false;
        voice.volume = volume;
        return true;
    }
    default:
        return false;
    }
}

std::uint32_t StateFlags(const VoiceMix& voice) {
    return voice.playing ? StateInUse | StatePlaying : 0;
}

void Render(const std::vector<VoiceMix*>& voices, const std::function<VoiceMix*(std::uint64_t)>& resolve,
            const Ngs2RenderBufferInfo* buffers, const std::uint32_t numBuffers, const std::uint32_t systemRate) {
    std::uint32_t grain = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t index = 0; index < numBuffers; ++index) {
        const Ngs2RenderBufferInfo& buffer = buffers[index];
        if (buffer.waveform_type != WaveformPcmS16 || buffer.num_channels == 0 || buffer.num_channels > MaxRenderChannels) continue;
        grain = std::min<std::uint32_t>(grain, static_cast<std::uint32_t>(buffer.buffer_size / (buffer.num_channels * sizeof(std::int16_t))));
    }
    if (grain == std::numeric_limits<std::uint32_t>::max() || grain == 0) return;
    for (VoiceMix* voice : voices) {
        voice->input.assign(static_cast<std::size_t>(grain) * 2, 0.0f);
        voice->output.assign(static_cast<std::size_t>(grain) * 2, 0.0f);
    }
    for (VoiceMix* voice : voices) {
        switch (voice->rackId) {
        case RackSampler:
            renderSampler(*voice, grain, systemRate);
            break;
        case RackSubmixer:
        case RackMastering:
            if (voice->playing)
                for (std::size_t index = 0; index < voice->output.size(); ++index) voice->output[index] = voice->input[index] * voice->volume;
            break;
        default:
            // Reverb and unknown racks are not modelled: they contribute nothing rather than a guessed effect.
            break;
        }
        if (voice->rackId == RackMastering) writeMastering(*voice, buffers, numBuffers, grain);
        for (const Patch& patch : voice->patches) {
            VoiceMix* destination = resolve(patch.destHandle);
            if (destination == nullptr || destination == voice) continue;
            for (std::size_t index = 0; index < voice->output.size(); ++index) destination->input[index] += voice->output[index];
        }
    }
}

} // namespace ngs2mix
