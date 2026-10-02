#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <prx/libc/include/General.hpp>

#include "SDL.h"
#include "SceTypes.hpp"
#include "prx/libkernel/Time/include/Time.hpp"

static constexpr int PORT_TYPE_MAIN = 0;
static constexpr int PORT_TYPE_BGM = 1;
static constexpr int PORT_TYPE_VOICE = 2;
static constexpr int PORT_TYPE_PERSONAL = 3;
static constexpr int PORT_TYPE_PADSPK = 4;
static constexpr int PORT_TYPE_VIBRATION = 10;
static constexpr int PORT_TYPE_AUDIO3D = 126;
static constexpr int PORT_TYPE_AUX = 127;

static constexpr int PORTS_MAX = 32;
static constexpr int DEFAULT_VOLUME = 32768;
static constexpr std::uint32_t FORMAT_MASK = 0xFFu;
static constexpr std::uint64_t TARGET_LATENCY_US = 40000;
static constexpr std::uint64_t DRAIN_TIMEOUT_US = 200000;
static constexpr std::uint64_t DRAIN_SLEEP_US = 1000;

enum class Format {
    Unknown,
    S16Mono,
    S16Stereo,
    S16_8Ch,
    F32Mono,
    F32Stereo,
    F32_8Ch,
    S16_8ChStd,
    F32_8ChStd,
};

static bool formatIsFloat(Format f) {
    return f == Format::F32Mono || f == Format::F32Stereo ||
           f == Format::F32_8Ch || f == Format::F32_8ChStd;
}

static bool formatIsStd(Format f) {
    return f == Format::S16_8ChStd || f == Format::F32_8ChStd;
}

static int channelsForFormat(Format f) {
    switch (f) {
        case Format::S16Mono:
        case Format::F32Mono:
            return 1;
        case Format::S16Stereo:
        case Format::F32Stereo:
            return 2;
        case Format::S16_8Ch:
        case Format::F32_8Ch:
        case Format::S16_8ChStd:
        case Format::F32_8ChStd:
            return 8;
        default:
            throw std::runtime_error("channelsForFormat: unknown format");
    }
}

static SDL_AudioFormat sdlFormat(Format f) {
    return formatIsFloat(f) ? AUDIO_F32SYS : AUDIO_S16SYS;
}

static std::uint32_t bytesPerSample(Format f) {
    return formatIsFloat(f) ? sizeof(float) : sizeof(std::int16_t);
}

struct Port {
    bool used = false;
    int type = 0;
    std::uint32_t samplesNum = 0;
    std::uint32_t freq = 0;
    Format format = Format::Unknown;
    int channels = 0;
    int volume[8] = {};
    std::uint64_t lastOutputTime = 0;
    SDL_AudioDeviceID device = 0;
    SDL_AudioSpec spec = {};
    bool deviceStarted = false;
    std::uint64_t tracePushes = 0;
    std::uint64_t traceUnderruns = 0;
};

static std::mutex g_mutex;
static Port g_ports[PORTS_MAX];
static bool g_sdlInitialized = false;

static bool ensureSdlAudio() {
    if (g_sdlInitialized) {
        return true;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        return false;
    }
    g_sdlInitialized = true;
    return true;
}

static bool openDevice(Port& port) {
    if (!ensureSdlAudio()) {
        return false;
    }
    SDL_AudioSpec desired{};
    desired.freq = static_cast<int>(port.freq);
    desired.format = sdlFormat(port.format);
    desired.channels = static_cast<Uint8>(port.channels);
    desired.samples = static_cast<Uint16>(port.samplesNum);
    desired.callback = nullptr;
    SDL_AudioSpec obtained{};
    port.device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, SDL_AUDIO_ALLOW_ANY_CHANGE);
    if (port.device == 0) {
        return false;
    }
    port.spec = obtained;
    if (std::getenv("APS5_TRACE_AUDIOOUT") != nullptr) {
        std::fprintf(stderr, "[audioout] device %u: wanted %d Hz fmt 0x%x ch %u samples %u, got %d Hz fmt 0x%x ch %u samples %u\n",
                     port.device, desired.freq, desired.format, desired.channels, desired.samples,
                     obtained.freq, obtained.format, obtained.channels, obtained.samples);
    }
    // Queue the target latency before starting playback; otherwise callback phase alone
    // can repeatedly starve a port receiving 256-frame blocks in a 480-frame device.
    SDL_PauseAudioDevice(port.device, 1);
    port.deviceStarted = false;
    return true;
}

static void closeDevice(Port& port) {
    if (port.device != 0 && SDL_WasInit(SDL_INIT_AUDIO) != 0) {
        SDL_ClearQueuedAudio(port.device);
        SDL_CloseAudioDevice(port.device);
    }
    port.device = 0;
    port.spec = {};
    port.deviceStarted = false;
}

static constexpr std::uint32_t STD_8CH_MAP[8] = {0, 1, 2, 3, 6, 7, 4, 5};

static const void* prepareBuffer(const Port& port, const void* data, std::vector<std::uint8_t>& buf) {
    const auto frames = port.samplesNum;
    const auto ch = static_cast<std::uint32_t>(port.channels);
    const auto bps = bytesPerSample(port.format);
    const auto size = frames * ch * bps;

    bool volumeChanged = false;
    for (std::uint32_t i = 0; i < ch; i++) {
        if (port.volume[i] != DEFAULT_VOLUME) {
            volumeChanged = true;
            break;
        }
    }

    if (!volumeChanged && !formatIsStd(port.format)) {
        return data;
    }

    buf.resize(size);
    const bool isStd = formatIsStd(port.format) && ch == 8;

    if (formatIsFloat(port.format)) {
        auto* dst = reinterpret_cast<float*>(buf.data());
        const auto* src = static_cast<const float*>(data);
        for (std::uint32_t fr = 0; fr < frames; fr++) {
            for (std::uint32_t c = 0; c < ch; c++) {
                const auto srcCh = isStd ? STD_8CH_MAP[c] : c;
                dst[fr * ch + c] = src[fr * ch + srcCh] *
                    (static_cast<float>(port.volume[c]) / static_cast<float>(DEFAULT_VOLUME));
            }
        }
    } else {
        auto* dst = reinterpret_cast<std::int16_t*>(buf.data());
        const auto* src = static_cast<const std::int16_t*>(data);
        for (std::uint32_t fr = 0; fr < frames; fr++) {
            for (std::uint32_t c = 0; c < ch; c++) {
                const auto srcCh = isStd ? STD_8CH_MAP[c] : c;
                std::int64_t s = static_cast<std::int64_t>(src[fr * ch + srcCh]) *
                    port.volume[c] / DEFAULT_VOLUME;
                s = std::clamp(s,
                    static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::min()),
                    static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::max()));
                dst[fr * ch + c] = static_cast<std::int16_t>(s);
            }
        }
    }
    return buf.data();
}

// Debug aid: APS5_TRACE_AUDIOOUT=1 reports, about once a second per port, the block pointer and the
// peak of the 16-bit samples the title outputs (whether its audio is silent before any mixing).
static void traceOutput(int handle, const Port& port, const void* data) {
    static const bool enabled = std::getenv("APS5_TRACE_AUDIOOUT") != nullptr;
    if (!enabled || data == nullptr || bytesPerSample(port.format) != 2) return;
    static std::uint64_t counts[PORTS_MAX] = {};
    static int peaks[PORTS_MAX] = {};
    const auto index = static_cast<std::size_t>(handle - 1) % PORTS_MAX;
    const auto* samples = static_cast<const std::int16_t*>(data);
    for (std::uint32_t i = 0; i < port.samplesNum * static_cast<std::uint32_t>(port.channels); ++i)
        peaks[index] = std::max(peaks[index], std::abs(static_cast<int>(samples[i])));
    // The first minute of each port is also kept as raw S16 in audioout_port<handle>.raw.
    static std::FILE* files[PORTS_MAX] = {};
    static std::uint64_t written[PORTS_MAX] = {};
    const std::uint64_t bytes = static_cast<std::uint64_t>(port.samplesNum) * static_cast<std::uint32_t>(port.channels) * 2u;
    if (written[index] < 60ull * port.freq * static_cast<std::uint32_t>(port.channels) * 2u) {
        if (files[index] == nullptr) {
            char name[32];
            std::snprintf(name, sizeof(name), "audioout_port%d.raw", handle);
            files[index] = std::fopen(name, "wb");
        }
        if (files[index] != nullptr) { std::fwrite(data, 1, bytes, files[index]); std::fflush(files[index]); }
        written[index] += bytes;
    }
    const auto blocksPerSecond = port.samplesNum != 0 ? port.freq / port.samplesNum : 1u;
    if (++counts[index] % std::max(1u, blocksPerSecond) != 0) return;
    std::fprintf(stderr, "[audioout] port %d type %d %u ch %d: block %p, peak %d over the last second\n", handle, port.type, port.samplesNum, port.channels, data, peaks[index]);
    peaks[index] = 0;
}

static void queueAudio(Port& port, const void* data) {
    // Without an SDL device there is nothing to queue: the callers already sleep for the block's
    // duration so the game's timing holds. A null pointer is the documented way to wait until the
    // port's queued audio has been output (sceAudioOutOutput(handle, NULL)); it queues nothing.
    if (port.device == 0) return;
    if (data == nullptr) {
        if (!port.deviceStarted && SDL_GetQueuedAudioSize(port.device) != 0) {
            SDL_PauseAudioDevice(port.device, 0);
            port.deviceStarted = true;
        }
        const std::uint64_t waitStart = sceKernelGetProcessTime();
        while (SDL_GetQueuedAudioSize(port.device) > 0) {
            if (sceKernelGetProcessTime() - waitStart > DRAIN_TIMEOUT_US) {
                if (std::getenv("APS5_TRACE_AUDIOOUT") != nullptr)
                    std::fprintf(stderr, "[audioout] device %u queue timeout: %u bytes\n", port.device, SDL_GetQueuedAudioSize(port.device));
                SDL_ClearQueuedAudio(port.device);
                break;
            }
            KernelTimespec req{};
            req.tv_sec = 0;
            req.tv_nsec = static_cast<long>(DRAIN_SLEEP_US * 1000ULL);
            sceKernelNanosleep(&req, nullptr);
        }
        return;
    }

    std::vector<std::uint8_t> prepareBuf;
    const void* prepared = prepareBuffer(port, data, prepareBuf);
    const std::uint32_t preparedSize = port.samplesNum *
        static_cast<std::uint32_t>(port.channels) * bytesPerSample(port.format);

    SDL_AudioCVT cvt{};
    const int cvtResult = SDL_BuildAudioCVT(
        &cvt,
        sdlFormat(port.format), static_cast<Uint8>(port.channels), static_cast<int>(port.freq),
        port.spec.format, port.spec.channels, port.spec.freq);

    if (cvtResult < 0) {
        throw std::runtime_error(std::string("SDL_BuildAudioCVT: ") + SDL_GetError());
    }

    const void* queueData = prepared;
    std::uint32_t queueSize = preparedSize;
    std::vector<std::uint8_t> convertBuf;

    if (cvtResult > 0) {
        convertBuf.resize(static_cast<std::size_t>(preparedSize) * cvt.len_mult);
        std::memcpy(convertBuf.data(), prepared, preparedSize);
        cvt.buf = convertBuf.data();
        cvt.len = static_cast<int>(preparedSize);
        if (SDL_ConvertAudio(&cvt) < 0) {
            throw std::runtime_error(std::string("SDL_ConvertAudio: ") + SDL_GetError());
        }
        queueData = cvt.buf;
        queueSize = static_cast<std::uint32_t>(cvt.len_cvt);
    }

    const std::uint64_t bufferUs = port.freq != 0
        ? (1000000ULL * port.samplesNum) / port.freq
        : 0;
    const std::uint32_t buffers = bufferUs != 0
        ? static_cast<std::uint32_t>((TARGET_LATENCY_US + bufferUs - 1) / bufferUs)
        : 2u;
    const std::uint32_t minQueued = queueSize * std::clamp(buffers, 2u, 16u);
    const std::uint64_t waitStart = sceKernelGetProcessTime();

    while (SDL_GetQueuedAudioSize(port.device) > minQueued) {
        if (sceKernelGetProcessTime() - waitStart > DRAIN_TIMEOUT_US) {
            if (std::getenv("APS5_TRACE_AUDIOOUT") != nullptr)
                std::fprintf(stderr, "[audioout] device %u queue cleared after drain timeout\n", port.device);
            SDL_ClearQueuedAudio(port.device);
            break;
        }
        KernelTimespec req{};
        req.tv_sec = 0;
        req.tv_nsec = static_cast<long>(DRAIN_SLEEP_US * 1000ULL);
        sceKernelNanosleep(&req, nullptr);
    }

    if (std::getenv("APS5_TRACE_AUDIOOUT") != nullptr) {
        const auto queued = SDL_GetQueuedAudioSize(port.device);
        if (port.tracePushes++ > 200 && port.deviceStarted && queued == 0 && port.traceUnderruns++ < 40) {
            std::fprintf(stderr, "[audioout] device %u underrun at %llu us (waited %llu us)\n", port.device,
                         static_cast<unsigned long long>(sceKernelGetProcessTime()),
                         static_cast<unsigned long long>(sceKernelGetProcessTime() - waitStart));
        }
        if (port.tracePushes % 1024 == 0)
            std::fprintf(stderr, "[audioout] device %u pushes %llu underruns %llu queued %u bytes\n", port.device,
                         static_cast<unsigned long long>(port.tracePushes),
                         static_cast<unsigned long long>(port.traceUnderruns), queued);
    }
    if (SDL_QueueAudio(port.device, queueData, queueSize) < 0) {
        throw std::runtime_error(std::string("SDL_QueueAudio: ") + SDL_GetError());
    }
    if (!port.deviceStarted && SDL_GetQueuedAudioSize(port.device) >= minQueued) {
        SDL_PauseAudioDevice(port.device, 0);
        port.deviceStarted = true;
    }
    // APS5_LOG_OUT("device=%u type=%d bytes=%u queued=%u", port.device, port.type, queueSize, SDL_GetQueuedAudioSize(port.device));
}

static bool portTypeValid(int type) {
    return (type >= PORT_TYPE_MAIN && type <= PORT_TYPE_PADSPK) ||
           type == PORT_TYPE_VIBRATION ||
           type == PORT_TYPE_AUDIO3D ||
           type == PORT_TYPE_AUX;
}

static Port* getPort(int handle) {
    const int idx = handle - 1;
    if (idx < 0 || idx >= PORTS_MAX || !g_ports[idx].used) {
        return nullptr;
    }
    return &g_ports[idx];
}

extern "C" {

int APS5_VABI sceAudioOutInit() {
    return 0;
}

int APS5_VABI sceAudioOutOpen(int userId, int type, int index, std::uint32_t len,
    std::uint32_t freq, std::uint32_t param) {
    (void)userId;
    if (!portTypeValid(type)) {
        return -2144993270;
    }
    if (len == 0) {
        return -2144993274;
    }
    if (freq == 0) {
        return -2144993272;
    }
    if (index != 0) {
        throw std::runtime_error("sceAudioOutOpen: index != 0 not supported");
    }

    Format format = Format::Unknown;
    switch (param & FORMAT_MASK) {
        case 0: format = Format::S16Mono; break;
        case 1: format = Format::S16Stereo; break;
        case 2: format = Format::S16_8Ch; break;
        case 3: format = Format::F32Mono; break;
        case 4: format = Format::F32Stereo; break;
        case 5: format = Format::F32_8Ch; break;
        case 6: format = Format::S16_8ChStd; break;
        case 7: format = Format::F32_8ChStd; break;
        default:
            throw std::runtime_error("sceAudioOutOpen: unknown format param");
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < PORTS_MAX; i++) {
        if (!g_ports[i].used) {
            Port& port = g_ports[i];
            port.used = true;
            port.type = type;
            port.samplesNum = len;
            port.freq = freq;
            port.format = format;
            port.channels = channelsForFormat(format);
            port.lastOutputTime = 0;
            for (int c = 0; c < port.channels; c++) {
                port.volume[c] = DEFAULT_VOLUME;
            }
            if (type != PORT_TYPE_VIBRATION) {
                openDevice(port);
            }
            return i + 1;
        }
    }
    return -2144993275;
}

int APS5_VABI sceAudioOutClose(int handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Port* port = getPort(handle);
    if (port == nullptr) {
        return -2144993277;
    }
    closeDevice(*port);
    *port = Port{};
    return 0;
}

int APS5_VABI sceAudioOutOutput(int handle, const void* ptr) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Port* port = getPort(handle);
    if (port == nullptr) {
        return -2144993277;
    }

    const std::uint64_t blockUs = (1000000ULL * port->samplesNum) / port->freq;
    const std::uint64_t now = sceKernelGetProcessTime();
    const std::uint64_t next = port->lastOutputTime + blockUs;
    if (next > now && port->device == 0) {
        const std::uint64_t waitUs = next - now;
        // The native Windows nanosleep can round a 5.3 ms block up to a scheduler tick.
        // A vibration port shares the producer with audible ports, so use the kernel's precise wait.
        KernelTimespec req{};
        req.tv_sec = static_cast<time_t>(waitUs / 1000000ULL);
        req.tv_nsec = static_cast<long>((waitUs % 1000000ULL) * 1000ULL);
        sceKernelNanosleep(&req, nullptr);
        if (std::getenv("APS5_TRACE_AUDIOOUT") != nullptr) {
            static unsigned traced = 0;
            if (traced++ < 8) std::fprintf(stderr, "[audioout] port %d no-device wait requested %llu us actual %llu us\n", handle,
                                          static_cast<unsigned long long>(waitUs),
                                          static_cast<unsigned long long>(sceKernelGetProcessTime() - now));
        }
    }

    traceOutput(handle, *port, ptr);
    queueAudio(*port, ptr);
    // Keep the no-device cadence anchored to its deadline, rather than accumulating
    // mixer work and sleep overshoot on every block alongside the audible ports.
    port->lastOutputTime = port->device == 0 ? std::max(now, next) : sceKernelGetProcessTime();
    return static_cast<int>(port->samplesNum);
}

int APS5_VABI sceAudioOutOutputs(AudioOutOutputParam* param, std::uint32_t num) {
    if (param == nullptr || num == 0) {
        return -2144993276;
    }

    std::lock_guard<std::mutex> lock(g_mutex);

    for (std::uint32_t i = 0; i < num; i++) {
        if (getPort(param[i].handle) == nullptr) {
            return -2144993277;
        }
    }

    Port& first = *getPort(param[0].handle);
    const std::uint64_t blockUs = (1000000ULL * first.samplesNum) / first.freq;
    const std::uint64_t now = sceKernelGetProcessTime();

    std::uint64_t maxWait = 0;
    for (std::uint32_t i = 0; i < num; i++) {
        Port& p = *getPort(param[i].handle);
        const std::uint64_t next = p.lastOutputTime + blockUs;
        const std::uint64_t wait = next > now ? next - now : 0;
        if (wait > maxWait) {
            maxWait = wait;
        }
    }

    bool anyDevice = false;
    for (std::uint32_t i = 0; i < num; i++) {
        if (getPort(param[i].handle)->device != 0) {
            anyDevice = true;
            break;
        }
    }

    if (maxWait != 0 && !anyDevice) {
        KernelTimespec req{};
        req.tv_sec = static_cast<time_t>(maxWait / 1000000ULL);
        req.tv_nsec = static_cast<long>((maxWait % 1000000ULL) * 1000ULL);
        sceKernelNanosleep(&req, nullptr);
    }

    for (std::uint32_t i = 0; i < num; i++) {
        if (auto* port = getPort(param[i].handle)) { traceOutput(param[i].handle, *port, param[i].ptr); queueAudio(*port, param[i].ptr); }
    }

    const std::uint64_t done = sceKernelGetProcessTime();
    for (std::uint32_t i = 0; i < num; i++) {
        if (auto* port = getPort(param[i].handle))
            port->lastOutputTime = anyDevice ? done : std::max(now, port->lastOutputTime + blockUs);
    }

    return static_cast<int>(first.samplesNum);
}

int APS5_VABI sceAudioOutSetVolume(int handle, std::uint32_t flag, int* vol) {
    if (vol == nullptr) {
        return -2144993276;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    Port* port = getPort(handle);
    if (port == nullptr) {
        return -2144993277;
    }
    const bool isStd = formatIsStd(port->format);
    for (int i = 0; i < port->channels; i++, flag >>= 1u) {
        if ((flag & 1u) == 0) {
            continue;
        }
        int srcIdx = i;
        if (isStd) {
            if (i == 4) srcIdx = 6;
            else if (i == 5) srcIdx = 7;
            else if (i == 6) srcIdx = 4;
            else if (i == 7) srcIdx = 5;
        }
        port->volume[i] = vol[srcIdx];
    }
    return 0;
}

int APS5_VABI sceAudioOutGetPortState(int handle, AudioOutPortState* state) {
    if (state == nullptr) {
        return -2144993276;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    Port* port = getPort(handle);
    if (port == nullptr) {
        return -2144993277;
    }
    state->rerouteCounter = 0;
    state->volume = 127;
    state->flag = 0;
    state->activeState = 0;
    state->reserved[0] = 0;
    switch (port->type) {
        case PORT_TYPE_MAIN:
        case PORT_TYPE_BGM:
        case PORT_TYPE_AUDIO3D:
            state->output = 1;
            state->channel = static_cast<std::uint8_t>(port->channels > 2 ? 2 : port->channels);
            break;
        case PORT_TYPE_VOICE:
        case PORT_TYPE_PERSONAL:
            state->output = 0x40;
            state->channel = 1;
            break;
        case PORT_TYPE_PADSPK:
        case PORT_TYPE_VIBRATION:
            state->output = 4;
            state->channel = 1;
            break;
        case PORT_TYPE_AUX:
            state->output = 0x80;
            state->channel = 0;
            break;
        default:
            throw std::runtime_error("sceAudioOutGetPortState: unknown port type");
    }
    return 0;
}

}
