#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_LANES_WORKERPOOL_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_LANES_WORKERPOOL_HPP

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace AgcDriver::DriverDetail::Lanes {

inline bool Enabled() {
    static const bool enabled = [] { const auto* value = std::getenv("APS5_GPU_LANES"); return value != nullptr && std::strcmp(value, "1") == 0; }();
    return enabled;
}

using Commit = std::function<void()>;
struct Chunk {
    std::uint32_t queue = 0;
    std::uint64_t ordinal = 0;
    std::function<Commit(std::size_t)> prepare;
};

class WorkerPool {
public:
    explicit WorkerPool(std::size_t count = 4, std::size_t capacity = 64) : capacity(std::max<std::size_t>(1, capacity)) {
        try {
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
        for (;;) {
            std::shared_ptr<Item> item;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return closing || !pending.empty(); });
                if (pending.empty()) return;
                item = std::move(pending.front());
                pending.pop_front();
            }
            Commit commit;
            std::exception_ptr failure;
            try { commit = item->chunk.prepare(index); } catch (...) { failure = std::current_exception(); }
            {
                std::lock_guard lock(mutex);
                item->commit = std::move(commit);
                item->failure = failure;
                item->ready = true;
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
                try { if (item->commit) item->commit(); } catch (...) { failure = std::current_exception(); }
            }
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
    std::thread submitter;
    bool closing = false;
};

}
#endif
