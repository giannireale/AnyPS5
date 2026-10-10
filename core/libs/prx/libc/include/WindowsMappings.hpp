#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSMAPPINGS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSMAPPINGS_HPP

#ifdef _WIN32
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <map>
#include <memory>
#include <vector>
#include <stdexcept>
#include <string>
#include <system_error>

namespace GuestArena {

class WindowsMappings {
public:
    static WindowsMappings& Get() {
        static WindowsMappings mappings;
        return mappings;
    }

    // Opt-in Collect sub-phase accounting (APS5_COLLECT_SUBPHASE=1): splits each
    // Collect walk into lock-wait, shared compare/copy, private GetWriteWatch,
    // coverage query, shared-page enumeration and arming (alias VirtualProtect).
    // Disabled by default: one static getenv per function, no counter touched.
    struct CollectSubphase {
        // Per-phase triples: timed sections / requested bytes / nanoseconds.
        // A "timed section" is one now()/now() pair recorded for that phase,
        // which is also the number of timer pairs its nanoseconds contain.
        std::uint64_t sharedCalls = 0;
        std::uint64_t sharedBytes = 0;
        std::uint64_t sharedNanoseconds = 0;
        std::uint64_t privateCalls = 0;
        std::uint64_t privateBytes = 0;
        std::uint64_t privateNanoseconds = 0;
        std::uint64_t coverageCalls = 0;
        std::uint64_t coverageBytes = 0;
        std::uint64_t coverageNanoseconds = 0;
        std::uint64_t lockCalls = 0;
        std::uint64_t lockBytes = 0;
        std::uint64_t lockNanoseconds = 0;
        std::uint64_t enumCalls = 0;      // pinned/seen page enumeration (arming path)
        std::uint64_t enumBytes = 0;
        std::uint64_t enumNanoseconds = 0;
        std::uint64_t armCalls = 0;       // alias loop + VirtualProtect (arming path)
        std::uint64_t armBytes = 0;
        std::uint64_t armNanoseconds = 0;
        // Per-walk work counts. Like the triples, every increment happens under
        // mutex, so no extra locking.
        std::uint64_t sharedPages = 0;     // shared pages walked (compare branch)
        std::uint64_t pinnedPages = 0;     // pinned pages enumerated
        std::uint64_t armedProtectCalls = 0;  // VirtualProtect calls while arming
        std::uint64_t memcmpBytes = 0;     // bytes compared on shared pages
        std::uint64_t memcpyBytes = 0;     // bytes copied on shared mismatch
        std::uint64_t returnedPages = 0;   // pages pushed into the caller buffer
        // Empty now()/now() pair, one per timed section: the control cost is
        // controlNanoseconds / controlCalls per section, and total timer
        // overhead is that times the sum of the *Calls fields above.
        std::uint64_t controlCalls = 0;
        std::uint64_t controlNanoseconds = 0;
    };
    static bool CollectSubphaseEnabled() {
        static const bool enabled = std::getenv("APS5_COLLECT_SUBPHASE") != nullptr;
        return enabled;
    }
    // Snapshot and reset in a single lock: taking totals and resetting in two
    // lock acquisitions would drop every increment a concurrent Collect makes
    // between them. Callers get per-window deltas without an offline sum.
    CollectSubphase CollectSubphaseDrain() {
        std::lock_guard lock(mutex);
        const auto snapshot = subphase;
        subphase = CollectSubphase{};
        return snapshot;
    }

    void* Reserve(void* address, std::size_t bytes) {
        return allocate(GetCurrentProcess(), address, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    }

    std::vector<std::pair<std::uintptr_t, std::size_t>> Commit(void* address, std::size_t bytes, DWORD protection, std::size_t granule, bool watched) {
        std::vector<std::pair<std::uintptr_t, std::size_t>> created;
        std::lock_guard lock(mutex);
        const auto end = reinterpret_cast<std::uintptr_t>(address) + bytes;
        for (auto cursor = reinterpret_cast<std::uintptr_t>(address); cursor < end;) {
            const auto memory = query(cursor);
            const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
            if (memory.State == MEM_RESERVE) {
                const auto limit = std::min(end, cursor + granule);
                auto placeholderEnd = stop;
                while (placeholderEnd < limit) {
                    const auto next = query(placeholderEnd);
                    if (next.State != MEM_RESERVE) break;
                    placeholderEnd = reinterpret_cast<std::uintptr_t>(next.BaseAddress) + next.RegionSize;
                }
                const auto size = std::min(limit, placeholderEnd) - cursor;
                reset(cursor, size);
                const DWORD flags = MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | (watched ? MEM_WRITE_WATCH : 0);
                if (!allocate(GetCurrentProcess(), reinterpret_cast<void*>(cursor), size, flags, protection, nullptr, 0)) fail("replace guest placeholder with private memory");
                if (!created.empty() && created.back().first + created.back().second == cursor) created.back().second += size;
                else created.emplace_back(cursor, size);
                cursor += size;
            } else {
                if (memory.State != MEM_COMMIT) throw std::runtime_error("guest memory is not committed");
                const auto mapped = views.find(cursor & ~(pageBytes - 1));
                if (mapped != views.end()) {
                    mapped->second.protection = protection;
                    mapped->second.armed = false;
                    invalidate(*mapped->second.page);
                }
                DWORD previous;
                if (!VirtualProtect(reinterpret_cast<void*>(cursor), stop - cursor, protection, &previous)) fail("protect guest memory");
                cursor = stop;
            }
        }
        return created;
    }

    void Reset(void* address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        reset(reinterpret_cast<std::uintptr_t>(address), bytes);
    }

    void Map(void* address, std::size_t bytes, HANDLE section, std::uint64_t offset, DWORD protection) {
        std::lock_guard lock(mutex);
        auto cursor = reinterpret_cast<std::uintptr_t>(address);
        reset(cursor, bytes);
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) fail("keep shared guest section");
        const auto owned = std::make_shared<Section>(duplicate);
        for (std::size_t done = 0; done < bytes; done += pageBytes) {
            split(cursor + done, pageBytes);
            void* page = reinterpret_cast<void*>(cursor + done);
            if (!map(section, GetCurrentProcess(), page, offset + done, pageBytes, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, nullptr, 0)) fail("map shared guest page");
            DWORD previous;
            if (!VirtualProtect(page, pageBytes, protection, &previous)) fail("protect shared guest page");
            const auto key = std::make_pair(reinterpret_cast<std::uintptr_t>(section), offset + done);
            auto shared = physical[key].lock();
            if (!shared) {
                shared = std::make_shared<SharedPage>();
                physical[key] = shared;
            }
            const auto base = cursor + done;
            shared->aliases.push_back(base);
            views.emplace(base, View{shared, protection, 0, false, owned, offset + done, 0});
            invalidate(*shared);
        }
    }

    void SetProtection(std::uintptr_t address, std::size_t bytes, DWORD protection) {
        std::lock_guard lock(mutex);
        for (auto it = views.lower_bound(address); it != views.end() && it->first < address + bytes; ++it) {
            it->second.protection = protection;
            it->second.armed = false;
            invalidate(*it->second.page);
        }
    }

    void Pin(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) {
            auto& page = *it->second.page;
            ++page.pins;
            for (const auto alias : page.aliases) {
                auto& view = views.at(alias);
                if (!view.armed) continue;
                DWORD previous;
                if (!VirtualProtect(reinterpret_cast<void*>(alias), pageBytes, view.protection, &previous)) fail("pin shared guest page writable");
                view.armed = false;
            }
            invalidate(page);
        }
    }

    void Unpin(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) {
            auto& page = *it->second.page;
            if (page.pins != 0) --page.pins;
            invalidate(page);
        }
    }

    bool HandleWrite(std::uintptr_t address) {
        std::lock_guard lock(mutex);
        const auto base = address & ~(pageBytes - 1);
        const auto found = views.find(base);
        if (found == views.end() || !writable(found->second.protection)) return false;
        auto& view = found->second;
        invalidate(*view.page);
        DWORD previous;
        if (!VirtualProtect(reinterpret_cast<void*>(base), pageBytes, view.protection, &previous)) fail("resume shared memory write");
        view.armed = false;
        return true;
    }

    bool BeginHostWrite(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        const auto first = views.lower_bound(address & ~(pageBytes - 1));
        const auto end = address + bytes;
        for (auto it = first; it != views.end() && it->first < end; ++it) {
            if (!writable(it->second.protection)) return false;
        }
        for (auto it = first; it != views.end() && it->first < end; ++it) {
            auto& view = it->second;
            ++view.hostWrites;
            invalidate(*view.page);
            if (!view.armed) continue;
            DWORD previous;
            if (!VirtualProtect(reinterpret_cast<void*>(it->first), pageBytes, view.protection, &previous)) fail("open shared memory to a host write");
            view.armed = false;
        }
        return true;
    }

    void EndHostWrite(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        const auto end = address + bytes;
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < end; ++it) {
            --it->second.hostWrites;
            invalidate(*it->second.page);
        }
    }

    void* MapAlias(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        const auto refuse = [&](const char* reason) {
            char text[192];
            std::snprintf(text, sizeof(text), "read-write alias of shared guest memory 0x%llx+0x%llx: %s", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), reason);
            return std::runtime_error(text);
        };
        if (address % pageBytes != 0 || bytes % pageBytes != 0 || bytes == 0) throw refuse("the range is not made of whole shared pages");
        auto view = views.find(address);
        if (view == views.end()) throw refuse("the range does not start at a shared view");
        const auto section = view->second.section;
        const auto offset = view->second.offset;
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        for (std::size_t done = 0; done < bytes; done += pageBytes, ++view) {
            if (view == views.end() || view->first != address + done || view->second.offset != offset + done) throw refuse("the range is not one contiguous run of views of a section");
            if (view->second.section != section && !sameSection(view->second.section->handle, section->handle)) throw refuse("the range spans several sections");
        }
        const auto lead = offset % system.dwAllocationGranularity;
        void* alias = map(section->handle, GetCurrentProcess(), nullptr, offset - lead, lead + bytes, 0, PAGE_READWRITE, nullptr, 0);
        if (alias == nullptr) {
            char text[160];
            std::snprintf(text, sizeof(text), "MapViewOfFile3 of a read-write alias of shared guest memory 0x%llx+0x%llx", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), text);
        }
        return static_cast<char*>(alias) + lead;
    }

    void UnmapAlias(void* alias) {
        if (alias == nullptr) return;
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const auto base = reinterpret_cast<std::uintptr_t>(alias) & ~(static_cast<std::uintptr_t>(system.dwAllocationGranularity) - 1);
        if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(base), 0)) fail("unmap shared guest alias");
    }

    bool Protection(std::uintptr_t address, std::uint32_t* protection) {
        std::lock_guard lock(mutex);
        const auto found = views.find(address & ~(pageBytes - 1));
        if (found == views.end()) return false;
        *protection = found->second.protection;
        return true;
    }

    bool Collect(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear) {
        const auto subphaseOn = CollectSubphaseEnabled();
        const auto lockStart = subphaseOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        std::lock_guard lock(mutex);
        if (subphaseOn) addSubphase(&CollectSubphase::lockCalls, &CollectSubphase::lockBytes, &CollectSubphase::lockNanoseconds, bytes, lockStart);
        if (subphaseOn) {
            // One empty now()/now() pair per Collect: the control cost of a single
            // timed section. Phase overhead is control per section times the number
            // of sections, i.e. the sum of the *Calls fields.
            const auto controlStart = std::chrono::steady_clock::now();
            ++subphase.controlCalls;
            subphase.controlNanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - controlStart).count());
        }
        // Diagnostic fallback: compare contents instead of causing write faults in guest code.
        // Set before process startup so no shared view is ever armed in this mode.
        static const bool compareShared = [] {
            const auto* value = std::getenv("APS5_COMPARE_SHARED_WRITES");
            return value != nullptr && std::strcmp(value, "1") == 0;
        }();
        static const bool noSharedWriteArming = std::getenv("APS5_NO_SHARED_WRITE_ARMING") != nullptr;
        const auto firstShared = views.lower_bound(address & ~(pageBytes - 1));
        if (!compareShared && noSharedWriteArming && firstShared != views.end() && firstShared->first < address + bytes) {
            *count = 0;
            return false;
        }
        const auto capacity = *count;
        *count = 0;
        const auto end = address + bytes;
        if (clear && !collectable(address, end)) return false;
        for (auto cursor = address; cursor < end;) {
            if (!compareShared) {
                const auto nextClean = cleanRanges.upper_bound(cursor);
                if (nextClean != cleanRanges.begin()) {
                    const auto clean = std::prev(nextClean);
                    if (cursor < clean->second) {
                        cursor = std::min(end, clean->second);
                        continue;
                    }
                }
            }
            const auto base = cursor & ~(pageBytes - 1);
            const auto found = views.find(base);
            if (found != views.end()) {
                auto& view = found->second;
                const auto stop = std::min(end, base + pageBytes);
                if (view.protection == PAGE_NOACCESS) return false;
                const bool pinned = view.page->pins != 0;
                if (subphaseOn && pinned) ++subphase.pinnedPages;
                if (compareShared && !pinned) {
                    const auto sharedStart = subphaseOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                    if (subphaseOn) ++subphase.sharedPages;
                    const auto* currentBytes = reinterpret_cast<const std::byte*>(base);
                    // comparedBytes only has a full snapshot after the first mismatch; before that
                    // the size test short-circuits and no memcmp runs, so count bytes only there.
                    const bool compareRuns = view.page->comparedBytes.size() == pageBytes;
                    if (subphaseOn && compareRuns) subphase.memcmpBytes += pageBytes;
                    if (!compareRuns || std::memcmp(view.page->comparedBytes.data(), currentBytes, pageBytes) != 0) {
                        view.page->comparedBytes.resize(pageBytes);
                        std::memcpy(view.page->comparedBytes.data(), currentBytes, pageBytes);
                        if (subphaseOn) subphase.memcpyBytes += pageBytes;
                        invalidate(*view.page);
                    }
                    const auto firstChunk = (cursor - base) / 4096;
                    const auto endChunk = (stop - base + 4095) / 4096;
                    // Bytes are the part of the page actually requested: cursor may start
                    // mid-page on the first chunk, so stop - base would overstate it.
                    const auto spanBytes = stop - cursor;
                    for (auto chunk = firstChunk; chunk < endChunk; ++chunk) {
                        if (view.comparedSeen[chunk] == view.page->generation) continue;
                        // Capacity truncation is still real shared work: time it before
                        // returning, otherwise truncated walks vanish from the counters.
                        if (*count == capacity) {
                            if (subphaseOn) addSubphase(&CollectSubphase::sharedCalls, &CollectSubphase::sharedBytes, &CollectSubphase::sharedNanoseconds, spanBytes, sharedStart);
                            return true;
                        }
                        pages[(*count)++] = reinterpret_cast<void*>(base + chunk * 4096);
                        if (subphaseOn) ++subphase.returnedPages;
                        if (clear) view.comparedSeen[chunk] = view.page->generation;
                    }
                    cursor = stop;
                    if (subphaseOn) addSubphase(&CollectSubphase::sharedCalls, &CollectSubphase::sharedBytes, &CollectSubphase::sharedNanoseconds, spanBytes, sharedStart);
                    continue;
                }
                if (pinned || view.seen != view.page->generation) {
                    // Enumeration: every page of the view is pushed, which is the
                    // production-path (no COMPARE_SHARED_WRITES) cost of this branch.
                    const auto enumStart = subphaseOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                    const auto spanBytes = stop - cursor;
                    const auto needed = (stop - cursor + 4095) / 4096;
                    if (needed > capacity - *count) {
                        // Batch fill: compute end once, avoid repeated bound check
                        const auto fillEnd = cursor + (capacity - *count) * 4096;
                        for (auto at = cursor; at < fillEnd; at += 4096) {
                            pages[(*count)++] = reinterpret_cast<void*>(at);
                            if (subphaseOn) ++subphase.returnedPages;
                        }
                        if (subphaseOn) addSubphase(&CollectSubphase::enumCalls, &CollectSubphase::enumBytes, &CollectSubphase::enumNanoseconds, spanBytes, enumStart);
                        return true;
                    }
                    // Full enumeration: compute end once
                    const auto endAt = stop;
                    for (auto at = cursor; at < endAt; at += 4096) {
                        pages[(*count)++] = reinterpret_cast<void*>(at);
                        if (subphaseOn) ++subphase.returnedPages;
                    }
                    if (subphaseOn) addSubphase(&CollectSubphase::enumCalls, &CollectSubphase::enumBytes, &CollectSubphase::enumNanoseconds, spanBytes, enumStart);
                }
                if (clear && !pinned) {
                    // Arming: the alias walk plus one VirtualProtect per unarmed alias.
                    const auto armStart = subphaseOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                    // Collect contiguous aliases with same protection for batched VirtualProtect
                    std::vector<std::uintptr_t> toArm;
                    DWORD currentProtection = 0;
                    std::uintptr_t rangeStart = 0;
                    std::size_t rangeBytes = 0;
                    auto flushRange = [&](std::uintptr_t start, std::size_t bytes, DWORD prot) {
                        if (bytes == 0) return;
                        DWORD previous;
                        if (!VirtualProtect(reinterpret_cast<void*>(start), bytes, prot, &previous)) fail("arm shared memory write tracking");
                        if (subphaseOn) ++subphase.armedProtectCalls;
                    };
                    for (const auto alias : view.page->aliases) {
                        auto& other = views.at(alias);
                        if (!writable(other.protection) || other.armed || other.hostWrites != 0) continue;
                        const DWORD prot = other.protection == PAGE_EXECUTE_READWRITE ? PAGE_EXECUTE_READ : PAGE_READONLY;
                        if (rangeBytes == 0) {
                            rangeStart = alias;
                            rangeBytes = pageBytes;
                            currentProtection = prot;
                        } else if (alias == rangeStart + rangeBytes && prot == currentProtection) {
                            rangeBytes += pageBytes;
                        } else {
                            flushRange(rangeStart, rangeBytes, currentProtection);
                            rangeStart = alias;
                            rangeBytes = pageBytes;
                            currentProtection = prot;
                        }
                        other.armed = true;
                    }
                    flushRange(rangeStart, rangeBytes, currentProtection);
                    view.seen = view.page->generation;
                    rememberClean(base, base + pageBytes);
                    if (subphaseOn) addSubphase(&CollectSubphase::armCalls, &CollectSubphase::armBytes, &CollectSubphase::armNanoseconds, pageBytes, armStart);
                }
                cursor = stop;
            } else {
                const auto covStart = subphaseOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                const auto memory = query(cursor);
                const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                if (subphaseOn) addSubphase(&CollectSubphase::coverageCalls, &CollectSubphase::coverageBytes, &CollectSubphase::coverageNanoseconds, stop - cursor, covStart);
                if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE) return false;
                ULONG_PTR available = capacity - *count;
                if (available == 0) return true;
                DWORD granularity = 0;
                const auto privStart = subphaseOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                if (GetWriteWatch(clear ? WRITE_WATCH_FLAG_RESET : 0, reinterpret_cast<void*>(cursor), stop - cursor, pages + *count, &available, &granularity) != 0) fail("collect private guest writes");
                if (subphaseOn) addSubphase(&CollectSubphase::privateCalls, &CollectSubphase::privateBytes, &CollectSubphase::privateNanoseconds, stop - cursor, privStart);
                *count += available;
                if (subphaseOn) subphase.returnedPages += available;
                if (*count == capacity) return true;
                cursor = stop;
            }
        }
        return true;
    }

private:
    bool collectable(std::uintptr_t address, std::uintptr_t end) {
        for (auto cursor = address; cursor < end;) {
            const auto base = cursor & ~(pageBytes - 1);
            if (const auto found = views.find(base); found != views.end()) {
                if (found->second.protection == PAGE_NOACCESS) return false;
                cursor = std::min(end, base + pageBytes);
                continue;
            }
            const auto memory = query(cursor);
            if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE) return false;
            cursor = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
        }
        return true;
    }

    static constexpr std::size_t pageBytes = 0x4000;
    struct SharedPage {
        std::uint64_t generation = 1;
        std::vector<std::uintptr_t> aliases;
        std::vector<std::byte> comparedBytes;
        std::uint32_t pins = 0;
    };
    struct Section {
        HANDLE handle;
        explicit Section(HANDLE handle) : handle(handle) {}
        Section(const Section&) = delete;
        Section& operator=(const Section&) = delete;
        ~Section() { CloseHandle(handle); }
    };
    struct View {
        std::shared_ptr<SharedPage> page;
        DWORD protection;
        std::uint64_t seen;
        bool armed;
        std::shared_ptr<Section> section;
        std::uint64_t offset;
        std::array<std::uint64_t, 4> comparedSeen{};
        std::uint32_t hostWrites;
    };
    void forgetClean(std::uintptr_t start, std::uintptr_t end) {
        auto it = cleanRanges.lower_bound(start);
        if (it != cleanRanges.begin() && std::prev(it)->second > start) --it;
        while (it != cleanRanges.end() && it->first < end) {
            const auto first = it->first;
            const auto last = it->second;
            it = cleanRanges.erase(it);
            if (first < start) cleanRanges.emplace(first, start);
            if (last > end) it = cleanRanges.emplace(end, last).first;
        }
    }

    void rememberClean(std::uintptr_t start, std::uintptr_t end) {
        auto it = cleanRanges.lower_bound(start);
        if (it != cleanRanges.begin() && std::prev(it)->second >= start) --it;
        while (it != cleanRanges.end() && it->first <= end) {
            start = std::min(start, it->first);
            end = std::max(end, it->second);
            it = cleanRanges.erase(it);
        }
        cleanRanges.emplace(start, end);
    }

    void invalidate(SharedPage& page) {
        ++page.generation;
        for (const auto alias : page.aliases) forgetClean(alias, alias + pageBytes);
    }

    bool sameSection(HANDLE first, HANDLE second) const {
        return compare != nullptr && compare(first, second);
    }

    static bool writable(DWORD protection) {
        return protection == PAGE_READWRITE || protection == PAGE_EXECUTE_READWRITE;
    }
    using AllocateFunction = PVOID (WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using MapFunction = PVOID (WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using UnmapFunction = BOOL (WINAPI*)(HANDLE, PVOID, ULONG);
    using CompareFunction = BOOL (WINAPI*)(HANDLE, HANDLE);

    WindowsMappings() {
        const auto module = GetModuleHandleW(L"KernelBase.dll");
        if (!module) fail("load Windows memory API");
        allocate = reinterpret_cast<AllocateFunction>(GetProcAddress(module, "VirtualAlloc2"));
        map = reinterpret_cast<MapFunction>(GetProcAddress(module, "MapViewOfFile3"));
        unmap = reinterpret_cast<UnmapFunction>(GetProcAddress(module, "UnmapViewOfFile2"));
        if (!allocate || !map || !unmap) throw std::runtime_error("Windows placeholder memory APIs are required");
        compare = reinterpret_cast<CompareFunction>(GetProcAddress(module, "CompareObjectHandles"));
    }

    [[noreturn]] static void fail(const char* operation) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
    }

    static MEMORY_BASIC_INFORMATION query(std::uintptr_t address) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) != sizeof(memory)) fail("query guest memory");
        return memory;
    }

    static void split(std::uintptr_t address, std::size_t bytes) {
        auto memory = query(address);
        memory = query(reinterpret_cast<std::uintptr_t>(memory.AllocationBase));
        if (memory.State != MEM_RESERVE) throw std::runtime_error("guest mapping requires a placeholder");
        const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (address != base) {
            if (!VirtualFree(reinterpret_cast<void*>(base), address - base, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("split guest placeholder prefix");
            memory = query(address);
        }
        if (memory.RegionSize < bytes) throw std::runtime_error("guest placeholder is too small");
        if (memory.RegionSize != bytes && !VirtualFree(reinterpret_cast<void*>(address), bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("split guest placeholder suffix");
    }

    void reset(std::uintptr_t address, std::size_t bytes) {
        const auto end = address + bytes;
        forgetClean(address, end);
        for (auto cursor = address; cursor < end;) {
            const auto memory = query(cursor);
            if (memory.State == MEM_RESERVE) {
                cursor = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                continue;
            }
            if (reinterpret_cast<std::uintptr_t>(memory.AllocationBase) != cursor) throw std::runtime_error("cannot release part of a host allocation");
            auto allocationEnd = cursor;
            do {
                const auto part = query(allocationEnd);
                if (part.AllocationBase != memory.AllocationBase) break;
                allocationEnd = reinterpret_cast<std::uintptr_t>(part.BaseAddress) + part.RegionSize;
            } while (allocationEnd < end);
            if (allocationEnd > end || query(allocationEnd).AllocationBase == memory.AllocationBase) throw std::runtime_error("guest release truncates a host allocation");
            if (memory.Type == MEM_MAPPED) {
                if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(cursor), MEM_PRESERVE_PLACEHOLDER)) fail("unmap shared guest page");
                const auto found = views.find(cursor);
                if (found != views.end()) {
                    std::erase(found->second.page->aliases, cursor);
                    views.erase(found);
                }
            } else if (memory.Type == MEM_PRIVATE) {
                if (!VirtualFree(reinterpret_cast<void*>(cursor), allocationEnd - cursor, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("release private guest memory");
            } else {
                throw std::runtime_error("unsupported guest mapping type");
            }
            cursor = allocationEnd;
        }
        const auto last = query(reinterpret_cast<std::uintptr_t>(query(end - 1).AllocationBase));
        const auto lastBase = reinterpret_cast<std::uintptr_t>(last.BaseAddress);
        if (lastBase + last.RegionSize > end) split(lastBase, end - lastBase);
        const auto first = query(address);
        split(address, std::min(bytes, reinterpret_cast<std::uintptr_t>(first.BaseAddress) + first.RegionSize - address));
        if (query(address).RegionSize != bytes && !VirtualFree(reinterpret_cast<void*>(address), bytes, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) fail("coalesce guest placeholders");
    }

    static void addSubphase(std::uint64_t CollectSubphase::*calls, std::uint64_t CollectSubphase::*bytes, std::uint64_t CollectSubphase::*nanoseconds, std::size_t span, std::chrono::steady_clock::time_point start) {
        auto& totals = Get().subphase;
        ++(totals.*calls);
        (totals.*bytes) += static_cast<std::uint64_t>(span);
        (totals.*nanoseconds) += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    }

    std::map<std::uintptr_t, std::uintptr_t> cleanRanges;
    std::map<std::uintptr_t, View> views;
    std::map<std::pair<std::uintptr_t, std::uint64_t>, std::weak_ptr<SharedPage>> physical;
    CollectSubphase subphase;
    std::mutex mutex;
    AllocateFunction allocate = nullptr;
    MapFunction map = nullptr;
    UnmapFunction unmap = nullptr;
    CompareFunction compare = nullptr;
};

}
#endif

#endif
