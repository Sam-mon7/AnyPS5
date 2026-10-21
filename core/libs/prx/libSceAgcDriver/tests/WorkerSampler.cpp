#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path);
    check(input.good(), "sampler output is missing");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void work(std::stop_token token) {
    std::uint64_t value = 1;
    while (!token.stop_requested()) {
        value = value * 6364136223846793005ull + 1;
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    check(value != 0, "invalid work result");
}

}

int main(int argc, char** argv) {
    try {
        AgcDriver::SetProfilingEnabled(false);
        check(!AgcDriver::ProfilingEnabled(), "profiling gate did not close");
#ifdef _WIN32
        check(argc == 2, "expected sampler output directory");
        const auto directory = std::filesystem::path(argv[1]) / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(directory);
        const auto worker = directory / "worker.txt";
        const auto process = directory / "threads.txt";
        _putenv_s("APS5_SAMPLE_WORKER", worker.string().c_str());
        _putenv_s("APS5_SAMPLE_THREADS", process.string().c_str());
        AgcDriver::StartWorkerSampler();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        AgcDriver::StopWorkerSampler();
        check(read(worker).starts_with("# 0 samples"), "disabled worker sampler collected samples");
        check(read(process).starts_with("# 0 rounds"), "disabled process sampler collected samples");
        std::vector<std::jthread> workers;
        for (unsigned index = 0; index < 2; ++index) workers.emplace_back([](std::stop_token token) { work(token); });
        AgcDriver::SetProfilingEnabled(true);
        AgcDriver::StartWorkerSampler();
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        AgcDriver::SetProfilingEnabled(false);
        workers.clear();
        AgcDriver::StopWorkerSampler();
        check(!read(worker).starts_with("# 0 samples"), "enabled worker sampler collected nothing");
        check(!read(process).starts_with("# 0 rounds"), "enabled process sampler collected nothing");
        unsigned stacks = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            const auto name = entry.path().filename().string();
            if (name.starts_with("threads.txt.thread-") && name.ends_with(".collapsed.txt") && read(entry.path()).find(';') != std::string::npos) ++stacks;
        }
        check(stacks >= 2, "process sampler did not export stacks for multiple threads");
#endif
        AgcDriver::SetProfilingEnabled(true);
        check(AgcDriver::ProfilingEnabled(), "profiling gate did not reopen");
        std::puts("worker sampler gating and process stacks passed");
        return 0;
    } catch (const std::exception& error) {
        AgcDriver::StopWorkerSampler();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
