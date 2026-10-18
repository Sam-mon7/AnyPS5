#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_LANES_SCHEDULER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_LANES_SCHEDULER_HPP

#include "WorkerPool.hpp"
#include <chrono>
#include <map>
#include <optional>

namespace AgcDriver::DriverDetail::Lanes {

struct Step {
    std::optional<Chunk> chunk;
    bool complete = false;
};

struct Parser {
    std::function<Step()> next;
    std::function<void(std::exception_ptr)> finish;
};

class Scheduler {
public:
    void Cancel(std::exception_ptr error) {
        std::map<std::uint32_t, std::deque<Parser>> cancelled;
        {
            std::lock_guard lock(mutex);
            cancelled.swap(pending);
        }
        for (auto& [queue, parsers] : cancelled) for (auto& parser : parsers) parser.finish(error);
    }
    void Enqueue(std::uint32_t queue, Parser parser) {
        std::lock_guard lock(mutex);
        pending[queue].push_back(std::move(parser));
        changed.notify_one();
    }

    void Run(const std::function<bool()>& stop, const Commit& poll, std::size_t workers = 4) {
        WorkerPool pool(workers);
        struct Active {
            Parser parser;
            std::future<void> inFlight;
        };
        std::map<std::uint32_t, Active> active;
        std::exception_ptr failure;
        std::uint64_t ordinal = 0;
        try {
            while (!stop()) {
                {
                    std::lock_guard lock(mutex);
                    for (auto& [queue, parsers] : pending) {
                        if (parsers.empty() || active.contains(queue)) continue;
                        active.emplace(queue, Active{std::move(parsers.front()), {}});
                        parsers.pop_front();
                    }
                }
                bool progress = false;
                for (auto it = active.begin(); it != active.end();) {
                    auto& current = it->second;
                    if (current.inFlight.valid()) {
                        if (current.inFlight.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
                        current.inFlight.get();
                    }
                    auto step = current.parser.next();
                    if (step.complete) {
                        current.parser.finish(nullptr);
                        it = active.erase(it);
                        progress = true;
                        continue;
                    }
                    if (step.chunk) {
                        step.chunk->queue = it->first;
                        step.chunk->ordinal = ++ordinal;
                        current.inFlight = pool.Enqueue(std::move(*step.chunk));
                        progress = true;
                    }
                    ++it;
                }
                poll();
                if (!progress) {
                    std::unique_lock lock(mutex);
                    changed.wait_for(lock, std::chrono::microseconds(200));
                }
            }
        } catch (...) { failure = std::current_exception(); }
        for (auto& [queue, current] : active) {
            if (current.inFlight.valid()) {
                try { current.inFlight.get(); } catch (...) { if (failure == nullptr) failure = std::current_exception(); }
            }
            if (failure == nullptr) failure = std::make_exception_ptr(std::runtime_error("GPU lanes stopped"));
            current.parser.finish(failure);
        }
        Cancel(failure != nullptr ? failure : std::make_exception_ptr(std::runtime_error("GPU lanes stopped")));
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    std::map<std::uint32_t, std::deque<Parser>> pending;
};

}
#endif
