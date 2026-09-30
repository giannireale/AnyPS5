#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/KernelErrors.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace {

struct SyncWaiter {
    std::condition_variable changed;
    bool awakened = false;
};

std::mutex syncMutex;
std::unordered_map<std::uintptr_t, std::list<std::shared_ptr<SyncWaiter>>> waitersByAddress;

template <typename TValue>
TValue ReadValue(const void* address) {
    auto* value = const_cast<TValue*>(static_cast<const TValue*>(address));
    return std::atomic_ref<TValue>(*value).load(std::memory_order_seq_cst);
}

void RemoveWaiter(std::uintptr_t address, const std::shared_ptr<SyncWaiter>& waiter) {
    const auto found = waitersByAddress.find(address);
    if (found == waitersByAddress.end()) return;
    auto& waiters = found->second;
    for (auto entry = waiters.begin(); entry != waiters.end(); ++entry) {
        if (*entry == waiter) {
            waiters.erase(entry);
            break;
        }
    }
    if (waiters.empty()) waitersByAddress.erase(found);
}

template <typename TValue>
int WaitOnAddress(const volatile TValue* address, TValue expected, const std::uint32_t* timeoutMicroseconds) {
    const auto numericAddress = reinterpret_cast<std::uintptr_t>(address);
    if (numericAddress == 0 || (numericAddress & (alignof(TValue) - 1)) != 0) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    if (ReadValue<TValue>(const_cast<const TValue*>(address)) != expected) return 0;

    std::uint32_t timeout = 0;
    if (timeoutMicroseconds != nullptr) {
        timeout = *timeoutMicroseconds;
        if (timeout == 0) return SCE_KERNEL_ERROR_ETIMEDOUT;
    }

    auto waiter = std::make_shared<SyncWaiter>();
    std::unique_lock lock(syncMutex);
    if (ReadValue<TValue>(const_cast<const TValue*>(address)) != expected) return 0;
    waitersByAddress[numericAddress].push_back(waiter);
    if (ReadValue<TValue>(const_cast<const TValue*>(address)) != expected) {
        RemoveWaiter(numericAddress, waiter);
        return 0;
    }

    if (timeoutMicroseconds == nullptr) {
        waiter->changed.wait(lock, [&] { return waiter->awakened; });
        return 0;
    }

    const bool awakened = waiter->changed.wait_for(
        lock,
        std::chrono::microseconds(timeout),
        [&] { return waiter->awakened; });
    if (!awakened) {
        RemoveWaiter(numericAddress, waiter);
        if (ReadValue<TValue>(const_cast<const TValue*>(address)) != expected) return 0;
    }
    return awakened ? 0 : SCE_KERNEL_ERROR_ETIMEDOUT;
}

}

#ifdef APS5_SYNC_ON_ADDRESS_TEST_ACCESS
namespace KernelSyncOnAddressTestAccess {

std::size_t WaiterCount(const void* address) {
    std::lock_guard lock(syncMutex);
    const auto found = waitersByAddress.find(reinterpret_cast<std::uintptr_t>(address));
    return found == waitersByAddress.end() ? 0 : found->second.size();
}
}
#endif


extern "C" {

int APS5_VABI sceKernelSyncOnAddressWait(const volatile void* address, std::uint32_t expected, const std::uint32_t* timeoutMicroseconds) {
    return WaitOnAddress(static_cast<const volatile std::uint32_t*>(address), expected, timeoutMicroseconds);
}

int APS5_VABI sceKernelSyncOnAddressWait32(const volatile void* address, std::uint32_t expected, const std::uint32_t* timeoutMicroseconds) {
    return sceKernelSyncOnAddressWait(address, expected, timeoutMicroseconds);
}

int APS5_VABI sceKernelSyncOnAddressWait64(const volatile void* address, std::uint64_t expected, const std::uint32_t* timeoutMicroseconds) {
    return WaitOnAddress(static_cast<const volatile std::uint64_t*>(address), expected, timeoutMicroseconds);
}

int APS5_VABI sceKernelSyncOnAddressWake(const volatile void* address, std::int32_t requested) {
    const auto numericAddress = reinterpret_cast<std::uintptr_t>(address);
    if (numericAddress == 0 || (numericAddress & (alignof(std::uint32_t) - 1)) != 0 || requested < 0) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    if (requested == 0) return 0;

    std::lock_guard lock(syncMutex);
    const auto found = waitersByAddress.find(numericAddress);
    if (found == waitersByAddress.end()) return 0;
    auto& waiters = found->second;
    const auto count = std::min<std::size_t>(static_cast<std::size_t>(requested), waiters.size());
    for (std::size_t index = 0; index < count; ++index) {
        auto waiter = waiters.front();
        waiters.pop_front();
        waiter->awakened = true;
        waiter->changed.notify_one();
    }
    if (waiters.empty()) waitersByAddress.erase(found);
    return 0;
}

}
