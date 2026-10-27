#include "prx/libSceAgcDriver/Execution/include/Timeline.hpp"
#include <sstream>
#include <stdexcept>

using namespace AgcDriver::Timeline;

void Check(bool condition) {
    if (!condition) throw std::runtime_error("timeline format test failed");
}

int main() {
    Buffer buffer(7);
    std::ostringstream output(std::ios::binary);
    Event event{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    buffer.Push(event);
    Check(buffer.Drain(output));
    const auto bytes = output.str();
    Check(bytes.size() == 80);
    for (std::size_t i = 0; i < 6; ++i) {
        Check(static_cast<unsigned char>(bytes[i * 8]) == i + 1);
        for (std::size_t j = 1; j < 8; ++j) Check(bytes[i * 8 + j] == 0);
    }
    for (std::size_t i = 0; i < 4; ++i) {
        Check(static_cast<unsigned char>(bytes[48 + i * 4]) == i + 7);
        for (std::size_t j = 1; j < 4; ++j) Check(bytes[48 + i * 4 + j] == 0);
    }
    Check(bytes[64] == 11 && bytes[72] == 12);
    for (std::size_t i = 0; i < Buffer::Capacity + 3; ++i) {
        event.id = i;
        buffer.Push(event);
    }
    output.str({});
    Check(buffer.Drain(output));
    Check(output.str().size() == (Buffer::Capacity + 1) * sizeof(Event));
    Event lost{};
    std::memcpy(&lost, output.str().data() + Buffer::Capacity * sizeof(Event), sizeof(lost));
    Check(lost.kind == static_cast<std::uint32_t>(Kind::Lost) && lost.a == 3);
    buffer.Push(event);
    output.str({});
    Check(buffer.Drain(output) && output.str().size() == sizeof(Event));

    Buffer concurrent(8);
    std::atomic<bool> done{false};
    constexpr std::size_t count = 200000;
    std::thread producer([&] {
        for (std::size_t i = 1; i <= count; ++i) concurrent.Push({i, i, i ^ 0xabcdefu, 0, 0, 0, 8, 3, 0, 0, 0, 0});
        done.store(true, std::memory_order_release);
    });
    std::ostringstream collected(std::ios::binary);
    while (!done.load(std::memory_order_acquire)) Check(concurrent.Drain(collected));
    producer.join();
    Check(concurrent.Drain(collected));
    const auto records = collected.str();
    std::size_t accounted = 0;
    std::uint64_t previous = 0;
    for (std::size_t i = 0; i < records.size(); i += sizeof(Event)) {
        Event value{};
        std::memcpy(&value, records.data() + i, sizeof(value));
        if (value.kind == static_cast<std::uint32_t>(Kind::Lost)) accounted += value.a;
        else {
            Check(value.id > previous && value.a == (value.id ^ 0xabcdefu));
            previous = value.id;
            ++accounted;
        }
    }
    Check(accounted == count);
    if (Enabled) {
        std::vector<std::filesystem::path> previous;
        if (std::filesystem::exists(Directory)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(Directory)) previous.push_back(entry.path());
        }
        Bind(0x38);
        DeferredOrigins.push_back({42, 0x37});
        {
            DeferredScope origins;
            Check(DeferredOrigins.empty());
            {
                auto context = origins.At(0);
                Check(Packet == 42 && Opcode == 0x37);
            }
            Check(Packet == 0 && Opcode == 0);
            DeferredOrigins.push_back({43, 0x49});
        }
        Check(DeferredOrigins.size() == 1 && DeferredOrigins[0].packet == 43);
        DeferredOrigins.clear();
        {
            PacketScope packet(0x15, 5, 9);
            Emit(Kind::Flip);
        }
        Output().Stop();
        bool found = false;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(Directory)) {
            if (entry.path().extension() != ".aps5tl" || std::find(previous.begin(), previous.end(), entry.path()) != previous.end()) continue;
            std::ifstream file(entry.path(), std::ios::binary);
            std::array<char, 16> header{};
            file.read(header.data(), header.size());
            Check(std::memcmp(header.data(), "APS5TL1\0", 8) == 0 && header[8] == 80 && header[12] == 1);
            for (const auto kind : {Kind::PacketBegin, Kind::Flip, Kind::PacketEnd}) {
                Event value{};
                file.read(reinterpret_cast<char*>(&value), sizeof(value));
                Check(static_cast<bool>(file) && value.kind == static_cast<std::uint32_t>(kind) && value.queue == 0x38 && value.opcode == 0x15);
            }
            Check(file.peek() == std::char_traits<char>::eof());
            found = true;
        }
        Check(found);
    } else {
        Check(Emit(Kind::Flip) == 0 && Local == nullptr);
    }
}
