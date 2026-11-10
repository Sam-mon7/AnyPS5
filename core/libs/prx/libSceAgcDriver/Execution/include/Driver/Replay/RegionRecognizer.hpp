#ifndef AGCDRIVER_DRIVER_REPLAY_REGIONRECOGNIZER_HPP
#define AGCDRIVER_DRIVER_REPLAY_REGIONRECOGNIZER_HPP

#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail::Replay {

enum class RegionMatch : std::uint8_t { New, Structure, Exact };

struct RegionReport {
    RegionMatch match = RegionMatch::New;
    std::uint32_t packets = 0;
    std::uint32_t draws = 0;
    std::uint32_t dispatches = 0;
    std::uint32_t patchWords = 0;
    std::uint32_t alternatingWords = 0;
    bool stablePatches = false;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> patchWordsByKind;
};

class RegionRecognizer {
public:
    RegionReport Observe(std::uint32_t queue, std::span<const std::uint32_t> commands);
    void EndFrame();

private:
    struct Region {
        std::vector<std::uint32_t> words;
        std::vector<std::uint32_t> patchSites;
        bool compared = false;
    };
    using Frame = std::map<std::uint32_t, std::vector<Region>>;

    static const Region* at(const Frame& frame, std::uint32_t queue, std::size_t index);

    Frame older;
    Frame previous;
    Frame current;
};

}

#endif
