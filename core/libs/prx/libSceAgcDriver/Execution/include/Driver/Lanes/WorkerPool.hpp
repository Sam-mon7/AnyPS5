#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_LANES_WORKERPOOL_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_LANES_WORKERPOOL_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <vector>

namespace AgcDriver::DriverDetail::Lanes {

inline bool Enabled() {
    static const bool enabled = [] { const auto* value = std::getenv("APS5_GPU_LANES"); return value != nullptr && std::strcmp(value, "1") == 0; }();
    return enabled;
}

struct PreparationCounts {
    std::uint64_t prepared = 0, parallel = 0, fallbacks = 0, busyNs = 0, idleNs = 0;
    auto operator<=>(const PreparationCounts&) const = default;
};

class PreparationProfile {
public:
    static bool Enabled() {
        static const bool profile = Lanes::Enabled() && std::getenv("APS5_PROFILE_DRAW") != nullptr;
        return profile;
    }
    static PreparationCounts Counts() {
        return {prepared.load(std::memory_order_relaxed), parallel.load(std::memory_order_relaxed), fallbacks.load(std::memory_order_relaxed), busyNs.load(std::memory_order_relaxed), idleNs.load(std::memory_order_relaxed)};
    }
    static std::string ProfileLine() {
        if (!Enabled()) return {};
        const auto counts = Counts();
        std::ostringstream line;
        line << "[gpu-lanes] cumulative prepared=" << counts.prepared << " prepared-parallel=" << counts.parallel << " proof-fallbacks=" << counts.fallbacks
             << " worker-busy-ns=" << counts.busyNs << " worker-idle-ns=" << counts.idleNs << '\n';
        return line.str();
    }
    static void ReportIfDue() {
        if (!Enabled()) return;
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        static std::atomic<std::int64_t> last{now};
        auto previous = last.load(std::memory_order_relaxed);
        if (now - previous < 10000 || !last.compare_exchange_strong(previous, now)) return;
        const auto line = ProfileLine();
        std::fputs(line.c_str(), stderr);
    }

private:
    friend class WorkerPool;
    inline static std::atomic<std::uint64_t> prepared{0}, parallel{0}, fallbacks{0}, busyNs{0}, idleNs{0};
};

using Commit = std::function<void()>;
struct Chunk {
    std::uint32_t queue = 0;
    std::uint64_t ordinal = 0;
    std::function<Commit(std::size_t)> prepare;
    std::function<bool()> validate;
    Commit fallback;
};

class WorkerPool {
public:
    explicit WorkerPool(std::size_t count = 4, std::size_t capacity = 64) : capacity(std::max<std::size_t>(1, capacity)) {
        try {
            preparing.resize(std::max<std::size_t>(1, count));
            for (std::size_t index = 0; index < std::max<std::size_t>(1, count); ++index) workers.emplace_back([this, index] { work(index); });
            submitter = std::thread([this] { submit(); });
        } catch (...) {
            close();
            throw;
        }
    }
    ~WorkerPool() { close(); }
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    std::future<void> Enqueue(Chunk chunk) {
        if (chunk.validate && !chunk.fallback) throw std::invalid_argument("GPU lanes proof validation requires an ordered fallback");
        auto item = std::make_shared<Item>();
        item->chunk = std::move(chunk);
        auto result = item->completion.get_future();
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return closing || ordered.size() < capacity; });
        if (closing) throw std::runtime_error("GPU lanes worker pool is closed");
        pending.push_back(item);
        ordered.push_back(std::move(item));
        changed.notify_all();
        return result;
    }

private:
    struct Item {
        Chunk chunk;
        Commit commit;
        std::exception_ptr failure;
        std::promise<void> completion;
        bool ready = false;
        bool fallback = false, parallel = false;
    };
    void close() {
        {
            std::lock_guard lock(mutex);
            closing = true;
        }
        changed.notify_all();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        if (submitter.joinable()) submitter.join();
    }
    void work(std::size_t index) {
        const bool profile = PreparationProfile::Enabled();
        for (;;) {
            std::shared_ptr<Item> item;
            {
                std::unique_lock lock(mutex);
                const auto idle = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                changed.wait(lock, [&] { return closing || !pending.empty(); });
                if (profile) PreparationProfile::idleNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - idle).count(), std::memory_order_relaxed);
                if (pending.empty()) return;
                item = std::move(pending.front());
                pending.pop_front();
                if (profile) {
                    for (auto* active : preparing) if (active != nullptr) active->parallel = item->parallel = true;
                    preparing[index] = item.get();
                }
            }
            Commit commit;
            std::exception_ptr failure;
            bool fallback = false, prepared = false;
            const auto busy = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            try {
                fallback = item->chunk.validate && !item->chunk.validate();
                if (!fallback) {
                    commit = item->chunk.prepare(index);
                    prepared = true;
                    fallback = item->chunk.validate && !item->chunk.validate();
                    if (fallback) commit = {};
                }
            } catch (...) { failure = std::current_exception(); }
            {
                std::lock_guard lock(mutex);
                item->commit = std::move(commit);
                item->failure = failure;
                item->fallback = fallback;
                item->ready = true;
                if (profile) {
                    preparing[index] = nullptr;
                    PreparationProfile::busyNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - busy).count(), std::memory_order_relaxed);
                    if (prepared) {
                        PreparationProfile::prepared.fetch_add(1, std::memory_order_relaxed);
                        if (item->parallel) PreparationProfile::parallel.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            changed.notify_all();
        }
    }
    void submit() {
        std::exception_ptr failure;
        for (;;) {
            std::shared_ptr<Item> item;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return (closing && ordered.empty()) || (!ordered.empty() && ordered.front()->ready); });
                if (ordered.empty()) return;
                item = ordered.front();
            }
            if (failure == nullptr) failure = item->failure;
            if (failure == nullptr) {
                try {
                    if (item->fallback || (item->chunk.validate && !item->chunk.validate())) {
                        if (PreparationProfile::Enabled()) PreparationProfile::fallbacks.fetch_add(1, std::memory_order_relaxed);
                        item->chunk.fallback();
                    } else if (item->commit) item->commit();
                } catch (...) { failure = std::current_exception(); }
            }
            PreparationProfile::ReportIfDue();
            if (failure != nullptr) item->completion.set_exception(failure);
            else item->completion.set_value();
            {
                std::lock_guard lock(mutex);
                ordered.pop_front();
            }
            changed.notify_all();
        }
    }
    std::size_t capacity;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Item>> pending, ordered;
    std::vector<std::thread> workers;
    std::vector<Item*> preparing;
    std::thread submitter;
    bool closing = false;
};

}
#endif
