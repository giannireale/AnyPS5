#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <stop_token>
#include <system_error>
#include <thread>
#include <vector>
#endif

namespace AgcDriver {

#ifdef _WIN32
namespace {

std::jthread processSamplerThread;
std::jthread workerSamplerThread;

// Debug aid: APS5_SAMPLE_WORKER=<file> samples the calling thread every millisecond and, every 20 s,
// writes "module+offset self inclusive" lines to <file> for offline symbolization with nm.
struct Sampler {
    HANDLE target = nullptr;
    std::string path;
    std::mutex mutex;
    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> counts;
    std::uint64_t samples = 0;
    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> intervalCounts;
    std::uint64_t intervalSamples = 0;
    std::uint64_t intervalEmptySamples = 0;
    std::uint64_t intervalFailedContexts = 0;
    bool collectIntervals = false;
    std::uint64_t created = 0;
    std::string name;

    ~Sampler() noexcept(false) {
        if (target != nullptr && !CloseHandle(target)) throw std::system_error(GetLastError(), std::system_category(), "Closing sampler thread handle");
    }

    void sample() {
        std::vector<std::uint64_t> frames;
        frames.reserve(12);
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (SuspendThread(target) == static_cast<DWORD>(-1)) return;
        const bool captured = GetThreadContext(target, &context) != 0;
        if (captured) {
            // Unwinding reads the target's stack while it is suspended; frames without unwind data
            // (JIT/guest code) end the walk.
            for (int depth = 0; depth < 12 && context.Rip != 0; ++depth) {
                frames.push_back(context.Rip);
                DWORD64 imageBase = 0;
                auto* entry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                if (entry == nullptr) break;
                PVOID handler = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, entry, &context, &handler, &establisher, nullptr);
            }
        }
        ResumeThread(target);
        recordFrames(frames, captured);
    }

    void recordFrames(const std::vector<std::uint64_t>& frames, bool captured) {
        std::lock_guard lock(mutex);
        ++samples;
        if (collectIntervals) {
            ++intervalSamples;
            if (frames.empty()) ++intervalEmptySamples;
            if (!captured) ++intervalFailedContexts;
        }
        for (std::size_t i = 0; i < frames.size(); ++i) {
            auto& count = counts[frames[i]];
            if (i == 0) ++count.first;
            ++count.second;
            if (collectIntervals) {
                auto& window = intervalCounts[frames[i]];
                if (i == 0) ++window.first;
                ++window.second;
            }
        }
    }

    void write() {
        std::lock_guard lock(mutex);
        std::FILE* file = std::fopen(path.c_str(), "w");
        if (file == nullptr) return;
        std::fprintf(file, "# %llu samples\n", static_cast<unsigned long long>(samples));
        for (const auto& [address, count] : counts) {
            HMODULE module = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module)) GetModuleFileNameA(module, name, sizeof(name));
            const char* base = std::strrchr(name, '\\');
            std::fprintf(file, "%s 0x%llx %llu %llu\n", base ? base + 1 : name, static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(module)), static_cast<unsigned long long>(count.first), static_cast<unsigned long long>(count.second));
        }
        std::fclose(file);
    }
};

std::uint64_t ThreadCreationKey(HANDLE target) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetThreadTimes(target, &created, &exited, &kernel, &user) == 0) return 0;
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32u) | created.dwLowDateTime;
}

std::string ThreadDisplayName(HANDLE target) {
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto query = [] {
        const HMODULE module = GetModuleHandleA("kernel32.dll");
        return module != nullptr
            ? reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(module, "GetThreadDescription"))
            : nullptr;
    }();
    if (query == nullptr) return {};
    PWSTR wide = nullptr;
    if (FAILED(query(target, &wide)) || wide == nullptr) return {};
    char narrow[64] = "?";
    const int written = WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
    LocalFree(wide);
    if (written <= 0) return {};
    narrow[sizeof(narrow) - 1] = '\0';
    return narrow;
}

std::string EscapedThreadName(const std::string& name, bool token = false) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    const auto limit = std::min<std::size_t>(name.size(), 128);
    for (std::size_t i = 0; i < limit; ++i) {
        const auto byte = static_cast<unsigned char>(name[i]);
        if (byte >= 0x20 && byte <= 0x7e && byte != '"' && byte != '\\' && (!token || byte != ' ')) {
            result += static_cast<char>(byte);
        } else {
            result += "\\x";
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }
    }
    if (name.size() > limit) result += "...";
    return result;
}

// Debug aid: APS5_SAMPLE_THREADS=<file> samples every thread in the process every 2 ms and, every
// 20 s, writes each busy thread's hottest frames ("module+offset self inclusive") to <file>.
struct ProcessSampler {
    std::string path;
    DWORD self = 0;
    std::map<DWORD, Sampler> threads;
    std::uint64_t rounds = 0;
    bool interval = false;
    std::chrono::steady_clock::time_point started{};
    std::chrono::steady_clock::time_point flushed{};
    std::uint64_t intervalRounds = 0;
    std::uint64_t intervalSequence = 1;
    struct Retired {
        DWORD id;
        std::uint64_t created;
        std::string name;
        std::uint64_t samples;
        std::uint64_t emptySamples;
        std::uint64_t failedContexts;
        std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> counts;
    };
    std::vector<Retired> retired;

    std::chrono::steady_clock::time_point enumerated{};

    void reapTerminatedThreads() {
        if (!interval) return;
        for (auto it = threads.begin(); it != threads.end();) {
            if (it->second.target != nullptr &&
                WaitForSingleObject(it->second.target, 0) == WAIT_OBJECT_0) {
                if (it->second.intervalSamples != 0) {
                    retired.push_back(Retired{it->first, it->second.created,
                        it->second.name, it->second.intervalSamples, it->second.intervalEmptySamples,
                        it->second.intervalFailedContexts, it->second.intervalCounts});
                }
                it = threads.erase(it);
            } else {
                ++it;
            }
        }
    }

    // The system-wide thread snapshot is slow (tens of ms); the thread list is refreshed every 5 s and
    // the rounds in between only sample the known threads.
    void enumerate() {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return;
        reapTerminatedThreads();
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        const DWORD process = GetCurrentProcessId();
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
            auto& thread = threads[entry.th32ThreadID];
            thread.collectIntervals = interval;
            if (thread.target == nullptr) {
                thread.target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION |
                    (interval ? SYNCHRONIZE : 0), FALSE, entry.th32ThreadID);
                if (thread.target != nullptr && interval) {
                    thread.created = ThreadCreationKey(thread.target);
                    if (thread.created == 0) {
                        CloseHandle(thread.target);
                        thread.target = nullptr;
                        continue;
                    }
                    thread.name = ThreadDisplayName(thread.target);
                }
            }
        }
        CloseHandle(snapshot);
        enumerated = std::chrono::steady_clock::now();
    }

    void sample() {
        if (threads.empty() || std::chrono::steady_clock::now() - enumerated > std::chrono::seconds(5)) enumerate();
        for (auto& [id, thread] : threads) {
            if (thread.target != nullptr) thread.sample();
        }
        ++rounds;
        if (interval) ++intervalRounds;
    }

    void write() {
        if (!interval) {
            std::FILE* file = std::fopen(path.c_str(), "w");
            if (file == nullptr) return;
            std::fprintf(file, "# %llu rounds\n", static_cast<unsigned long long>(rounds));
            for (auto& [id, thread] : threads) {
                std::lock_guard lock(thread.mutex);
                std::vector<std::pair<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>>> hot(thread.counts.begin(), thread.counts.end());
                std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
                std::uint64_t leaves = 0;
                for (const auto& [address, count] : hot) leaves += count.first;
                if (leaves == 0) continue;
                std::fprintf(file, "thread %lu samples %llu\n", static_cast<unsigned long>(id), static_cast<unsigned long long>(thread.samples));
                for (std::size_t i = 0; i < hot.size() && i < 40; ++i) {
                    const auto address = hot[i].first;
                    HMODULE module = nullptr;
                    char name[MAX_PATH] = "?";
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module)) GetModuleFileNameA(module, name, sizeof(name));
                    const char* base = std::strrchr(name, '\\');
                    std::fprintf(file, "  %s 0x%llx %llu %llu\n", base ? base + 1 : name, static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(module)), static_cast<unsigned long long>(hot[i].second.first), static_cast<unsigned long long>(hot[i].second.second));
                }
            }
            std::fclose(file);
            return;
        }
        writeInterval();
    }

    void writeInterval(std::FILE* suppliedFile = nullptr) {
        const auto now = std::chrono::steady_clock::now();
        const auto startMs = static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(flushed - started).count());
        const auto endMs = static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count());
        std::FILE* file = suppliedFile != nullptr ? suppliedFile : std::fopen(path.c_str(), "a");
        if (file == nullptr) return;
        const auto run = static_cast<unsigned long long>(started.time_since_epoch().count());
        const auto pid = static_cast<unsigned long>(GetCurrentProcessId());
        bool written = std::fprintf(file, "\n# interval startMs=%llu endMs=%llu cumulativeRounds=%llu intervalRounds=%llu pid=%lu run=%llu sequence=%llu\n",
            startMs, endMs, static_cast<unsigned long long>(rounds), static_cast<unsigned long long>(intervalRounds),
            pid, run, static_cast<unsigned long long>(intervalSequence)) >= 0;
        for (auto& [id, thread] : threads) {
            std::lock_guard lock(thread.mutex);
            written &= writeThreadBlock(file, id, thread.created, thread.name, thread.intervalSamples,
                thread.intervalEmptySamples, thread.intervalFailedContexts, thread.intervalCounts);
        }
        for (const auto& gone : retired) {
            written &= writeThreadBlock(file, gone.id, gone.created, gone.name, gone.samples,
                gone.emptySamples, gone.failedContexts, gone.counts);
        }
        if (written && std::ferror(file) == 0) {
            written &= std::fprintf(file, "# complete pid=%lu run=%llu sequence=%llu\n",
                pid, run, static_cast<unsigned long long>(intervalSequence)) >= 0;
        }
        const bool flushedOk = std::fflush(file) == 0;
        const bool streamOk = std::ferror(file) == 0;
        const bool closedOk = std::fclose(file) == 0;
        if (!written || !flushedOk || !streamOk || !closedOk) return;
        for (auto& [id, thread] : threads) {
            std::lock_guard lock(thread.mutex);
            thread.intervalCounts.clear();
            thread.intervalSamples = 0;
            thread.intervalEmptySamples = 0;
            thread.intervalFailedContexts = 0;
        }
        retired.clear();
        intervalRounds = 0;
        ++intervalSequence;
        flushed = now;
    }

    static bool writeThreadBlock(std::FILE* file, DWORD id, std::uint64_t created, const std::string& name,
        std::uint64_t samples, std::uint64_t emptySamples, std::uint64_t failedContexts,
        const std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>>& counts) {
        bool written = std::fprintf(file, "thread %lu created %llu name \"%s\" samples %llu empty %llu failedContext %llu\n", static_cast<unsigned long>(id),
            static_cast<unsigned long long>(created), EscapedThreadName(name).c_str(), static_cast<unsigned long long>(samples),
            static_cast<unsigned long long>(emptySamples), static_cast<unsigned long long>(failedContexts)) >= 0;
        for (const auto& [address, count] : counts) {
            if (count.first == 0 && count.second == 0) continue;
            HMODULE module = nullptr;
            char mod[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module)) GetModuleFileNameA(module, mod, sizeof(mod) - 1);
            const char* base = std::strrchr(mod, '\\');
            written &= std::fprintf(file, "  %s 0x%llx %llu %llu\n", EscapedThreadName(base ? base + 1 : mod, true).c_str(),
                static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(module)),
                static_cast<unsigned long long>(count.first), static_cast<unsigned long long>(count.second)) >= 0;
        }
        return written;
    }
};

bool IntervalCollectionEnabled() {
    const char* value = std::getenv("APS5_SAMPLE_THREADS_INTERVAL");
    return value != nullptr && std::string(value) == "1";
}

void StartProcessSampler() {
    const char* path = std::getenv("APS5_SAMPLE_THREADS");
    if (path == nullptr) return;
    const bool interval = IntervalCollectionEnabled();
    processSamplerThread = std::jthread([path = std::string(path), interval](std::stop_token token) {
        auto sampler = std::make_unique<ProcessSampler>();
        sampler->path = path;
        sampler->self = GetCurrentThreadId();
        sampler->interval = interval;
        sampler->started = std::chrono::steady_clock::now();
        sampler->flushed = sampler->started;
        auto flushed = std::chrono::steady_clock::now();
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            sampler->sample();
            if (std::chrono::steady_clock::now() - flushed > std::chrono::seconds(20)) {
                sampler->write();
                flushed = std::chrono::steady_clock::now();
            }
        }
        sampler->write();
    });
}

}

void StartWorkerSampler() {
    StartProcessSampler();
    const char* path = std::getenv("APS5_SAMPLE_WORKER");
    if (path == nullptr) return;
    auto sampler = std::make_unique<Sampler>();
    sampler->path = path;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sampler->target, THREAD_ALL_ACCESS, FALSE, 0)) throw std::system_error(GetLastError(), std::system_category(), "Duplicating sampler thread handle");
    workerSamplerThread = std::jthread([sampler = std::move(sampler)](std::stop_token token) {
        auto flushed = std::chrono::steady_clock::now();
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            sampler->sample();
            if (std::chrono::steady_clock::now() - flushed > std::chrono::seconds(20)) {
                sampler->write();
                flushed = std::chrono::steady_clock::now();
            }
        }
        sampler->write();
    });
}

void StopWorkerSampler() {
    processSamplerThread.request_stop();
    workerSamplerThread.request_stop();
    if (processSamplerThread.joinable()) processSamplerThread.join();
    if (workerSamplerThread.joinable()) workerSamplerThread.join();
}
#else
void StartWorkerSampler() {}
void StopWorkerSampler() {}
#endif

}
