#include "prx/libc/include/CriticalPathTrace.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {
void require(bool value) { if (!value) std::abort(); }
std::uint64_t number(const std::string& line, const char* key) {
    auto start = line.find(std::string("\"") + key + "\":");
    require(start != std::string::npos);
    start += std::char_traits<char>::length(key) + 3;
    return std::stoull(line.substr(start));
}
void rings() {
    CriticalPath::Detail::Ring<std::uint64_t, 4> ring;
    std::uint64_t value = 0;
    require(!ring.Pop(value));
    for (std::uint64_t n = 0; n < 1000; ++n) {
        for (std::uint64_t i = 0; i < 4; ++i) require(ring.Push(n * 4 + i));
        require(!ring.Push(99));
        for (std::uint64_t i = 0; i < 4; ++i) { require(ring.Pop(value)); require(value == n * 4 + i); }
        require(!ring.Pop(value));
    }
    std::thread producer([&] { for (std::uint64_t n = 0; n < 300000; ++n) while (!ring.Push(n)) std::this_thread::yield(); });
    for (std::uint64_t n = 0; n < 300000; ++n) { while (!ring.Pop(value)) std::this_thread::yield(); require(value == n); }
    producer.join();
    require(ring.Push(1, 1)); require(ring.Push(2, 1)); require(ring.Push(3, 1));
    require(!ring.Push(4, 1)); require(ring.Push(4));
    for (std::uint64_t i = 1; i <= 4; ++i) { require(ring.Pop(value)); require(value == i); }
}
std::string output() {
#ifdef _WIN32
    auto pid = _getpid();
#else
    auto pid = getpid();
#endif
    const char* directory = std::getenv("APS5_TRACE_DIR");
    auto path = std::filesystem::path(directory && *directory ? directory : ".") / ("aps5-critical-path-" + std::to_string(pid) + ".jsonl");
    std::ifstream stream(path);
    require(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}
int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "ring";
    if (mode == "ring") { rings(); return 0; }
    if (mode == "off" || mode == "invalid" || mode == "bad-window") { require(!CriticalPath::Enabled()); require(!CriticalPath::Capturing()); require(!CriticalPath::Emit(CriticalPath::Kind::Point, "disabled")); return 0; }
    require(CriticalPath::Enabled());
    if (mode == "default-window") {
        require(!CriticalPath::Capturing());
        CriticalPath::Name("startup-main");
        CriticalPath::Scope crossing("never-record-startup-phase");
        CriticalPath::Wait waiting("never-record-startup-wait");
        for (unsigned i = 0; i < 800; ++i) {
            require(CriticalPath::Emit(CriticalPath::Kind::Point, "never-record-startup-payload") == 0);
            require(CriticalPath::Present(i) == 0);
        }
        require(CriticalPath::Present(800) != 0);
        require(CriticalPath::Capturing());
        crossing.End(); waiting.End();
        std::thread named([] { CriticalPath::Name("window-worker"); CriticalPath::Emit(CriticalPath::Kind::Point, "work"); });
        named.join();
        for (unsigned i = 801; i <= 860; ++i) {
            for (unsigned n = 0; n < 1000; ++n) require(CriticalPath::Emit(CriticalPath::Kind::PhaseMark, "filtered-mark") == 0);
            require(CriticalPath::Present(i) != 0);
        }
        require(!CriticalPath::Enabled());
        require(!CriticalPath::Capturing());
        const auto text = output();
        require(text.find("never-record") == std::string::npos);
        require(text.find("filtered-mark") == std::string::npos);
        require(text.find("startup-main") != std::string::npos && text.find("window-worker") != std::string::npos);
        require(text.find("\"capture_start_index\":800,\"capture_length\":60") != std::string::npos);
        std::size_t count = 0, pos = 0;
        while ((pos = text.find(",\"present\",\"present\",", pos)) != std::string::npos) { ++count; ++pos; }
        require(count == 61);
        require(text.ends_with("{\"type\":\"footer\",\"complete\":true}\n"));
        std::puts("Default window retained names and exactly 60 intervals without startup payload");
        return 0;
    }
    if (mode == "cap") {
        require(CriticalPath::Present(0) != 0);
        std::vector<std::thread> writers;
        for (unsigned i = 0; i < 4; ++i) writers.emplace_back([] {
            while (CriticalPath::Enabled()) {
                for (unsigned n = 0; n < 1024; ++n) CriticalPath::Emit(CriticalPath::Kind::Point, "bounded-sustained-load");
                std::this_thread::yield();
            }
        });
        for (auto& writer : writers) writer.join();
        CriticalPath::Shutdown();
        const auto text = output();
        require(text.size() < 257 * 1024 * 1024);
        require(text.find("\"cap_reached\":true") != std::string::npos);
        require(text.ends_with("{\"type\":\"footer\",\"complete\":true}\n"));
        std::puts("Hard byte cap closed safely with concurrent producers and no process termination");
        return 0;
    }
    if (mode == "natural-return") {
        CriticalPath::Name("linked-return");
        CriticalPath::Present(1);
        { CriticalPath::Scope scope("linked-scope"); CriticalPath::Emit(CriticalPath::Kind::Point, "linked-point"); }
        CriticalPath::Present(2);
        return 0;
    }
    if (mode == "autoclose") {
        CriticalPath::Name("capture-main");
        CriticalPath::Scope open("crosses_capture_end");
        for (unsigned i = 0; i < 4; ++i) {
            require(CriticalPath::Enabled());
            require((CriticalPath::Present(100 + i) != 0) == (i >= 1));
        }
        require(!CriticalPath::Enabled());
        require(!CriticalPath::Capturing());
        const auto completed = output();
        require(completed.ends_with("{\"type\":\"footer\",\"complete\":true}\n"));
        require(completed.find("\"capture_start_index\":1,\"capture_length\":2") != std::string::npos);
        require(CriticalPath::Present(104) == 0);
        open.End();
        require(output() == completed);
        std::puts("Automatic capture completed before process exit");
        std::fflush(stdout);
        std::_Exit(0);
    }
    if (mode == "export") {
        CriticalPath::Present(41);
        CriticalPath::Name("main\"\\\n");
        char name[] = "copied";
        CriticalPath::Emit(CriticalPath::Kind::Point, name);
        name[0] = 'X';
        auto release = CriticalPath::Emit(CriticalPath::Kind::Release, "source", 7);
        CriticalPath::Emit(CriticalPath::Kind::Handoff, "accepted-work", 7, 0, release);
        CriticalPath::Wait wait("wait", 7);
        wait.End(release, "released");
        { CriticalPath::Wait unknown("unknown", 8); }
        { CriticalPath::Scope scope("scope"); scope.Mark("mark"); }
        CriticalPath::Emit(CriticalPath::Kind::Point, std::string(120, 'a').c_str());
        CriticalPath::Present(42);
    } else if (mode == "overflow") {
        require(CriticalPath::Present(0) != 0);
        for (unsigned n = 0; n < 400000; ++n) require(CriticalPath::Emit(CriticalPath::Kind::Point, "burst") != 0);
    } else if (mode == "limit") {
        require(CriticalPath::Present(0) != 0);
        for (unsigned n = 0; n < 132; ++n) {
            std::thread thread([n] { require((CriticalPath::Emit(CriticalPath::Kind::Point, "thread") != 0) == (n < 127)); });
            thread.join();
        }
    } else if (mode == "shutdown") {
        require(CriticalPath::Present(0) != 0);
        std::atomic<unsigned> started{0};
        std::atomic<bool> done{false};
        std::vector<std::thread> writers;
        for (unsigned i = 0; i < 12; ++i) writers.emplace_back([&] {
            CriticalPath::Name("writer");
            started.fetch_add(1);
            while (!done.load()) CriticalPath::Emit(CriticalPath::Kind::Point, "race");
        });
        while (started.load() != 12) std::this_thread::yield();
        std::thread stop1([] { CriticalPath::Shutdown(); });
        std::thread stop2([] { CriticalPath::Shutdown(); });
        stop1.join(); stop2.join(); done.store(true);
        for (auto& writer : writers) writer.join();
    } else std::abort();
    CriticalPath::Shutdown();
    require(!CriticalPath::Enabled());
    require(!CriticalPath::Capturing());
    require(CriticalPath::Present() == 0);
    const auto text = output();
    require(text.find("\"format\":\"aps5-critical-path/1\"") != std::string::npos);
    require(text.find("\"clock\":\"steady_ns\",\"clock_shared\":true") != std::string::npos);
    require(text.ends_with("{\"type\":\"footer\",\"complete\":true}\n"));
    std::array<std::uint64_t, 129> ids{}, times{}, counts{};
    std::uint64_t waitBegin = 0, releaseId = 0;
    for (std::size_t start = 0; start < text.size();) {
        auto end = text.find('\n', start);
        require(end != std::string::npos);
        const auto line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.front() == '[') {
            std::vector<std::string> fields;
            bool quoted = false, escaped = false;
            std::size_t begin = 1;
            for (std::size_t n = 1; n < line.size(); ++n) {
                const char c = line[n];
                if (escaped) { escaped = false; continue; }
                if (quoted && c == '\\') { escaped = true; continue; }
                if (c == '"') quoted = !quoted;
                if (!quoted && (c == ',' || c == ']')) { fields.push_back(line.substr(begin, n - begin)); begin = n + 1; }
            }
            require(fields.size() == 9);
            const auto id = std::stoull(fields[0]);
            const auto tid = std::stoull(fields[1]);
            const auto ns = std::stoull(fields[2]);
            require(tid > 0 && tid < ids.size());
            require((id >> 48) == tid && id > ids[tid] && ns >= times[tid]);
            ids[tid] = id; times[tid] = ns; ++counts[tid];
            if (fields[3] == "\"release\"") releaseId = id;
            if (fields[3] == "\"wait_begin\"") waitBegin = id;
            if (fields[3] == "\"handoff\"") require(std::stoull(fields[7]) == releaseId);
            if (fields[3] == "\"wait_end\"") {
                require(std::stoull(fields[6]) == waitBegin);
                if (fields[4] == "\"released\"") require(std::stoull(fields[7]) == releaseId);
                else require(std::stoull(fields[7]) == 0);
            }
        } else if (line.find("\"type\":\"thread_stats\"") != std::string::npos) {
            auto tid = number(line, "tid");
            require(tid > 0 && tid < counts.size());
            require(number(line, "emitted") == counts[tid]);
        }
    }
    if (mode == "export") {
        require(text.find("main\\\"\\\\\\u000a") != std::string::npos);
        require(text.find("\"copied\"") != std::string::npos);
        require(text.find(",true]") != std::string::npos);
        require(text.find("\"truncated\":1") != std::string::npos);
        require(text.find("\"wait_end\"") != std::string::npos);
    }
    if (mode == "overflow") {
        const auto start = text.find("{\"type\":\"thread_stats\"");
        require(start != std::string::npos);
        const auto stats = text.substr(start, text.find('\n', start) - start);
        require(number(stats, "dropped") > 0);
        require(number(stats, "emitted") + number(stats, "dropped") == 400001);
    }
    if (mode == "limit") require(text.find("\"unregistered_events\":5") != std::string::npos);
    std::puts("critical-path test passed");
    return 0;
}
