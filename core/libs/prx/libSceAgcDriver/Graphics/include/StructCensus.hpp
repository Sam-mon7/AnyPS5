#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_STRUCTCENSUS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_STRUCTCENSUS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// APS5_ARCHB_STRUCT_REPEAT_CENSUS (diagnostic, read once, default off; Stage 3 slice 7): hashes the
// structural state of every recorded draw and committed dispatch and compares each frame with the
// previous one, to choose between cross-frame pass replay, per-draw structure reuse and parallel
// secondary recording. One "[struct-census]" line every 10 s.
namespace AgcDriver::Graphics::StructCensus {

// What a commit's hash is made of. Levels nest: Shape (pipeline, pass key, framebuffer, set layouts,
// push stages, draw kind) < Structure (+ dynamic state, draw parameters, push bytes, resource
// template) < Addresses (+ guest addresses of the per-draw snapshots, index and indirect buffers) <
// Full (+ snapshot contents).
enum Level : std::size_t { LevelShape, LevelStructure, LevelAddresses, LevelFull, Levels };
enum Component : std::size_t { ComponentDynamic, ComponentParams, ComponentPush, ComponentBindings, ComponentAddresses, ComponentData, Components };
enum class Kind : std::uint8_t { Draw, Dispatch };

struct Commit {
    Kind kind = Kind::Draw;
    // A draw that began a render pass of its own (not continuing the previous draw's).
    bool passBegin = false;
    std::uint64_t shape = 0;
    std::array<std::uint64_t, Components> components{};
};

struct Stats {
    std::uint64_t frames = 0;  // frames compared with their predecessor
    std::uint64_t commits = 0;
    std::uint64_t draws = 0;
    std::uint64_t dispatches = 0;
    std::array<std::uint64_t, Levels> repeated{};     // commits whose level hash is in the previous frame (multiset)
    std::array<std::uint64_t, Levels> inLongRuns{};   // commits in runs of >= 8 matching the previous frame in order
    std::uint64_t passes = 0;
    std::uint64_t passDraws = 0;
    std::array<std::uint64_t, Levels> passesRepeated{};
    std::array<std::uint64_t, Levels> passDrawsRepeated{};  // draws inside repeated passes
    std::uint64_t aligned = 0;                               // commits aligned with a previous-frame commit of equal shape
    std::array<std::uint64_t, Components> alignedSame{};     // ... whose component is equal too
};

// Frame-by-frame comparison (pure logic; the unit tests drive it directly).
class Tracker {
public:
    static constexpr std::size_t LongRun = 8;
    void Note(const Commit& commit);
    // Ends the current frame: compares it with the previous one (if any) into the totals.
    void EndFrame();
    const Stats& Totals() const { return totals; }
    void ResetTotals() { totals = {}; }
    static std::array<std::uint64_t, Levels> LevelHashes(const Commit& commit);

private:
    struct Entry {
        std::array<std::uint64_t, Levels> levels;
        std::array<std::uint64_t, Components> components;
        Kind kind;
    };
    struct Frame {
        std::vector<Entry> entries;
        // Passes: [first, end) ranges of consecutive draws in one render pass.
        std::vector<std::pair<std::size_t, std::size_t>> passes;
        bool passOpen = false;
    };
    Frame current, previous;
    bool havePrevious = false;
    Stats totals;
};

bool Enabled();
std::uint64_t Mix(std::uint64_t hash, std::uint64_t value);
std::uint64_t HashBytes(std::uint64_t hash, std::span<const std::byte> bytes);
// Called on queue 0 at a flip (the pipeline is drained there, so every earlier commit is noted).
void NoteFrame();
// Thread-safe; a commit noted after NoteFrame belongs to the next frame.
void NoteCommit(const Commit& commit);
std::string Describe(const Stats& stats);

// APS5_ARCHB_DRAW_REUSE_CENSUS (diagnostic, read once, default off; Stage 3 Appendix B step B0):
// for every draw the queue-0 committer records, whether a per-draw structure-reuse cache keyed by
// (structure hash, occurrence index in the frame) would hit, i.e. the previous frame drew the same
// structure (pipeline, pass, framebuffer, set layouts, push stages, draw kind, dynamic state, draw
// parameters, push bytes, resource template) at least k+1 times, and the committer time of the
// draw's stages split by would-hit and miss: what structure reuse could remove (lookup, bindings,
// record, keep) next to what it keeps (targets, proof, access lists, pass bookkeeping). Recorder
// counters reuse_census_* per frame and a "[reuse-census]" line on stderr every 10 s.
enum ReuseStage : std::size_t { ReuseOther, ReuseTargets, ReuseLookup, ReuseProof, ReuseAccess, ReuseBindings, ReusePass, ReuseRecord, ReuseKeep, ReuseStages };
inline constexpr const char* ReuseStageNames[ReuseStages] = {"other", "targets", "lookup", "proof", "access", "bindings", "pass", "record", "keep"};

// Would-hit classification (pure logic; the unit tests drive it directly).
class ReuseTracker {
public:
    // A draw of `structure` in the current frame: whether the previous frame had its occurrence.
    bool Note(std::uint64_t structure);
    void EndFrame();

private:
    std::unordered_map<std::uint64_t, std::uint32_t> previous, current;
    bool havePrevious = false;
};

bool ReuseCensusEnabled();
// One recorded committer draw; `frame` is the flip count (a change ends the census frame).
void NoteReuseDraw(std::uint64_t structure, bool recipe, const std::array<std::uint64_t, ReuseStages>& ns, std::uint64_t totalNs, std::uint64_t frame);

}

#endif
