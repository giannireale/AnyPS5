#include <cstdint>
#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {

constexpr int SCE_AUDIO3D_ERROR_INVALID_PORT = static_cast<int>(0x80EA0002);
constexpr int SCE_AUDIO3D_ERROR_INVALID_PARAMETER = static_cast<int>(0x80EA0004);
constexpr int SCE_AUDIO3D_ERROR_NOT_READY = static_cast<int>(0x80EA0005);
constexpr int SCE_AUDIO3D_ERROR_NOT_SUPPORTED = static_cast<int>(0x80EA0007);
constexpr int SCE_AUDIO3D_ERROR_OUT_OF_RESOURCES = static_cast<int>(0x80EA0009);

constexpr std::uint32_t DefaultGranularity = 256;
constexpr std::uint32_t DefaultRate = 48000;
constexpr std::uint32_t DefaultMaxObjects = 512;
constexpr std::uint32_t DefaultQueueDepth = 2;
constexpr std::uint32_t DefaultNumBeds = 2;
constexpr std::uint32_t MaxQueueDepth = 8;
constexpr std::uint32_t MaxPorts = 4;
constexpr std::size_t MaxAttributeBytes = 4096;

struct Port {
    std::uint32_t id = 0;
    Audio3dOpenParameters parameters{};
    std::uint32_t queued = 0;
    std::uint64_t advanceCount = 0;
    std::map<std::uint32_t, std::vector<std::uint8_t>> attributes;
};

std::mutex g_lock;
bool g_initialised = false;
std::map<std::uint32_t, Port> g_ports;
std::uint32_t g_nextPort = 1;

Port* find(const std::uint32_t id) {
    const auto entry = g_ports.find(id);
    return entry == g_ports.end() ? nullptr : &entry->second;
}

}

extern "C" {

int APS5_VABI sceAudio3dInitialize(std::int64_t reserved) {
    if (reserved != 0) return SCE_AUDIO3D_ERROR_INVALID_PARAMETER;
    std::lock_guard lock(g_lock);
    g_initialised = true;
    return 0;
}

void APS5_VABI sceAudio3dGetDefaultOpenParameters(Audio3dOpenParameters* p) {
    if (p == nullptr) APS5_INVALID_ARG_EX;
    constexpr Audio3dOpenParameters defaults{0x20, 256, 0, 512, 2, 2, 0, 0};
    static_assert(offsetof(Audio3dOpenParameters, num_beds) == 0x20);
    std::memcpy(p, &defaults, defaults.size_this);
}

int APS5_VABI sceAudio3dPortOpen(int user_id, const Audio3dOpenParameters* parameters, std::uint32_t* id) {
    (void)user_id;
    if (parameters == nullptr || id == nullptr) return SCE_AUDIO3D_ERROR_INVALID_PARAMETER;
    if (parameters->size_this != 0x20 && parameters->size_this != sizeof(Audio3dOpenParameters)) return SCE_AUDIO3D_ERROR_INVALID_PARAMETER;
    if (parameters->granularity == 0) return SCE_AUDIO3D_ERROR_INVALID_PARAMETER;
    if (parameters->queue_depth == 0 || parameters->queue_depth > MaxQueueDepth) return SCE_AUDIO3D_ERROR_INVALID_PARAMETER;
    if (parameters->max_objects == 0) return SCE_AUDIO3D_ERROR_INVALID_PARAMETER;
    std::lock_guard lock(g_lock);
    if (!g_initialised) return SCE_AUDIO3D_ERROR_NOT_READY;
    if (g_ports.size() >= MaxPorts) return SCE_AUDIO3D_ERROR_OUT_OF_RESOURCES;
    Port port;
    port.id = g_nextPort++;
    std::memcpy(&port.parameters, parameters, parameters->size_this);
    if (port.parameters.rate == 0) port.parameters.rate = DefaultRate;
    if (parameters->size_this == 0x20) port.parameters.num_beds = DefaultNumBeds;
    const auto assigned = port.id;
    g_ports[assigned] = std::move(port);
    *id = assigned;
    return 0;
}

int APS5_VABI sceAudio3dPortPush(std::uint32_t port_id, std::uint32_t blocking) {
    std::lock_guard lock(g_lock);
    if (!g_initialised) return SCE_AUDIO3D_ERROR_NOT_READY;
    Port* port = find(port_id);
    if (port == nullptr) return SCE_AUDIO3D_ERROR_INVALID_PORT;
    if (port->queued >= port->parameters.queue_depth) {
        if (blocking == 0) return SCE_AUDIO3D_ERROR_NOT_READY;
        port->queued = port->parameters.queue_depth - 1;
    }
    ++port->queued;
    return 0;
}

int APS5_VABI sceAudio3dPortAdvance(std::uint32_t port_id) {
    std::lock_guard lock(g_lock);
    if (!g_initialised) return SCE_AUDIO3D_ERROR_NOT_READY;
    Port* port = find(port_id);
    if (port == nullptr) return SCE_AUDIO3D_ERROR_INVALID_PORT;
    if (port->queued == 0) return SCE_AUDIO3D_ERROR_NOT_READY;
    --port->queued;
    ++port->advanceCount;
    return 0;
}

int APS5_VABI sceAudio3dPortGetQueueLevel(std::uint32_t port_id, std::uint32_t* queue_level, std::uint32_t* queue_available) {
    std::lock_guard lock(g_lock);
    if (!g_initialised) return SCE_AUDIO3D_ERROR_NOT_READY;
    const Port* port = find(port_id);
    if (port == nullptr) return SCE_AUDIO3D_ERROR_INVALID_PORT;
    if (queue_level != nullptr) *queue_level = port->queued;
    if (queue_available != nullptr) *queue_available = port->parameters.queue_depth - port->queued;
    return 0;
}

int APS5_VABI sceAudio3dPortSetAttribute(std::uint32_t port_id, std::uint32_t attribute_id, const void* attribute, std::size_t attribute_size) {
    if (attribute == nullptr || attribute_size == 0) return SCE_AUDIO3D_ERROR_INVALID_PARAMETER;
    if (attribute_size > MaxAttributeBytes) return SCE_AUDIO3D_ERROR_NOT_SUPPORTED;
    std::lock_guard lock(g_lock);
    if (!g_initialised) return SCE_AUDIO3D_ERROR_NOT_READY;
    Port* port = find(port_id);
    if (port == nullptr) return SCE_AUDIO3D_ERROR_INVALID_PORT;
    const auto* bytes = static_cast<const std::uint8_t*>(attribute);
    port->attributes[attribute_id].assign(bytes, bytes + attribute_size);
    return 0;
}

}
