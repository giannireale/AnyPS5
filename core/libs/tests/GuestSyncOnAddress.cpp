#include "SceTypes.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

extern "C" {
int APS5_VABI sceKernelSyncOnAddressWait(const volatile void*, std::uint32_t, const std::uint32_t*);
int APS5_VABI sceKernelSyncOnAddressWait32(const volatile void*, std::uint32_t, const std::uint32_t*);
int APS5_VABI sceKernelSyncOnAddressWait64(const volatile void*, std::uint64_t, const std::uint32_t*);
int APS5_VABI sceKernelSyncOnAddressWake(const volatile void*, std::int32_t);
}

namespace KernelSyncOnAddressTestAccess {
std::size_t WaiterCount(const void* address);
}

static constexpr int SCE_KERNEL_ERROR_EINVAL = static_cast<int>(0x80020016);
static constexpr int SCE_KERNEL_ERROR_ETIMEDOUT = static_cast<int>(0x8002003C);

static void Require(bool value) {
    if (!value) std::abort();
}

static bool WaitForWaiters(const void* address, std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (KernelSyncOnAddressTestAccess::WaiterCount(address) == count) return true;
        std::this_thread::yield();
    }
    return false;
}

static bool WaitForReturned(const std::array<std::atomic<int>, 3>& results, std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        std::size_t returned = 0;
        for (const auto& result : results) returned += result.load() != -1;
        if (returned == count) return true;
        std::this_thread::yield();
    }
    return false;
}

int main() {
    alignas(8) std::uint32_t value32 = 7;
    Require(sceKernelSyncOnAddressWait(&value32, 8, nullptr) == 0);

    const std::uint32_t noWait = 0;
    Require(sceKernelSyncOnAddressWait32(&value32, value32, &noWait) == SCE_KERNEL_ERROR_ETIMEDOUT);
    const std::uint32_t finiteWait = 10000;
    Require(sceKernelSyncOnAddressWait32(&value32, value32, &finiteWait) == SCE_KERNEL_ERROR_ETIMEDOUT);
    Require(KernelSyncOnAddressTestAccess::WaiterCount(&value32) == 0);
    Require(sceKernelSyncOnAddressWait(nullptr, 0, nullptr) == SCE_KERNEL_ERROR_EINVAL);
    Require(sceKernelSyncOnAddressWait32(reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(&value32) + 1), 0, nullptr) == SCE_KERNEL_ERROR_EINVAL);
    Require(sceKernelSyncOnAddressWake(nullptr, 1) == SCE_KERNEL_ERROR_EINVAL);
    Require(sceKernelSyncOnAddressWake(&value32, -1) == SCE_KERNEL_ERROR_EINVAL);

    alignas(8) std::uint64_t value64 = 0x123456789abcdef0ULL;
    Require(sceKernelSyncOnAddressWait64(&value64, value64 ^ 1, nullptr) == 0);
    Require(sceKernelSyncOnAddressWait64(reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(&value64) + 4), value64, nullptr) == SCE_KERNEL_ERROR_EINVAL);

    alignas(4) std::uint32_t blockedValue = 3;
    std::array<std::atomic<int>, 3> results;
    for (auto& result : results) result.store(-1);
    std::array<std::thread, 3> threads;
    for (std::size_t index = 0; index < threads.size(); ++index) {
        threads[index] = std::thread([&, index] {
            results[index].store(sceKernelSyncOnAddressWait(&blockedValue, 3, nullptr));
        });
    }
    Require(WaitForWaiters(&blockedValue, threads.size()));
    Require(sceKernelSyncOnAddressWake(&blockedValue, 0) == 0);
    Require(KernelSyncOnAddressTestAccess::WaiterCount(&blockedValue) == threads.size());
    Require(sceKernelSyncOnAddressWake(&blockedValue, 1) == 0);
    Require(WaitForWaiters(&blockedValue, threads.size() - 1));
    Require(WaitForReturned(results, 1));
    Require(sceKernelSyncOnAddressWake(&blockedValue, INT32_MAX) == 0);
    for (auto& thread : threads) thread.join();
    for (const auto& result : results) Require(result.load() == 0);
    Require(KernelSyncOnAddressTestAccess::WaiterCount(&blockedValue) == 0);

    alignas(8) std::uint64_t blockedValue64 = 0x123456789abcdef0ULL;
    std::atomic<int> result64{-1};
    std::thread thread64([&] {
        result64.store(sceKernelSyncOnAddressWait64(&blockedValue64, blockedValue64, nullptr));
    });
    Require(WaitForWaiters(&blockedValue64, 1));
    Require(sceKernelSyncOnAddressWake(&blockedValue64, 1) == 0);
    const auto deadline64 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (result64.load() == -1 && std::chrono::steady_clock::now() < deadline64) std::this_thread::yield();
    Require(result64.load() == 0);
    thread64.join();
    Require(KernelSyncOnAddressTestAccess::WaiterCount(&blockedValue64) == 0);

}
