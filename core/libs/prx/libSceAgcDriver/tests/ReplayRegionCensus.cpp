#include "prx/libSceAgcDriver/Execution/include/Driver/Replay/RegionCensus.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace AgcDriver::DriverDetail::Replay;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<ResolvedItem> Stream(std::uint64_t address = 0x1000u) {
    ResolvedItem draw;
    draw.offset = 3;
    draw.kind = ResolvedKind::Draw;
    draw.prepared[static_cast<std::size_t>(ResolvedField::State)] = {10};
    draw.prepared[static_cast<std::size_t>(ResolvedField::Programs)] = {20, 21};
    draw.committed[static_cast<std::size_t>(ResolvedField::Buffers)] = {address, 64};
    ResolvedItem dispatch;
    dispatch.offset = 6;
    dispatch.kind = ResolvedKind::Dispatch;
    dispatch.committed[static_cast<std::size_t>(ResolvedField::ResourceKey)] = {30, 31};
    return {draw, dispatch};
}

void repeats() {
    const auto a = Stream();
    const auto b = Stream();
    Require(ResolvedSignature(a) == ResolvedSignature(b), "identical resolved streams have different signatures");
    Require(CompareResolved(a, b).equal, "identical resolved streams differ");
}

void alternates() {
    const auto a = Stream(0x1000);
    const auto b = Stream(0x2000);
    const auto c = Stream(0x1000);
    Require(!CompareResolved(a, b).equal && !CompareResolved(b, c).equal, "alternating addresses repeated at N-1");
    Require(CompareResolved(a, c).equal && ResolvedSignature(a) == ResolvedSignature(c), "a two-flight stream did not repeat at N-2");
}

void fields() {
    const auto before = Stream();
    for (std::size_t item = 0; item < before.size(); ++item) {
        for (std::size_t field = 0; field < ResolvedFieldNames.size(); ++field) {
            for (const bool committed : {false, true}) {
                auto after = before;
                auto& values = (committed ? after[item].committed : after[item].prepared)[field];
                if (values.empty()) values.push_back(1);
                else ++values[0];
                const auto difference = CompareResolved(before, after);
                Require(!difference.equal && difference.item == item && difference.field == field && difference.word == 0, "a field change was missed or attributed to another item/field");
                Require(ResolvedSignature(before) != ResolvedSignature(after), "a field change kept its signature");
            }
        }
    }
    const auto moved = CompareResolved(Stream(0x1000), Stream(0x2000));
    Require(moved.field == static_cast<std::size_t>(ResolvedField::Buffers) && moved.before == 0x1000 && moved.after == 0x2000, "a moved buffer was not reported with both addresses");
}

void positions() {
    const auto before = Stream();
    auto after = before;
    std::swap(after[0], after[1]);
    Require(!CompareResolved(before, after).equal && ResolvedSignature(before) != ResolvedSignature(after), "item order was erased");
    after = before;
    after.pop_back();
    Require(!CompareResolved(before, after).equal && CompareResolved(before, after).item == 1, "a missing item was not identified");
    Require(CompareResolved({}, {}).equal && !CompareResolved(before, {}).equal, "empty streams compare incorrectly");
}

void structure() {
    RegionRecognizer recognizer;
    const std::vector<std::uint32_t> commands{0xc0016900u, 0x80u, 1u, 0xc0012d00u, 3u, 2u};
    recognizer.Observe(0, commands);
    recognizer.EndFrame();
    auto longer = commands;
    longer.push_back(0x80000000u);
    const auto prefix = recognizer.Observe(0, longer);
    Require(prefix.match == RegionMatch::Prefix && prefix.prefixWords == commands.size(), "the recognizer did not retain the exact matched word boundary");
    recognizer.EndFrame();
    const auto exact = recognizer.Observe(0, longer);
    Require(exact.match == RegionMatch::Exact && exact.prefixWords == longer.size(), "an exact region lost its matched word boundary");
}

}

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "repeats") repeats();
        else if (mode == "alternates") alternates();
        else if (mode == "fields") fields();
        else if (mode == "positions") positions();
        else if (mode == "structure") structure();
        else throw std::runtime_error("unknown case");
        std::cout << "resolved region census: " << mode << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
