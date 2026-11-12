#include "prx/libSceAgcDriver/Graphics/include/GpuAuthorityCensus.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace C = AgcDriver::Graphics::GpuAuthorityCensus;

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

int main() {
    try {
        C::ResetForTest();
        C::Published(100, 200, C::Kind::CopyBack);
        C::GpuRead(100, 200, 0);
        C::CpuRead(150, 1, 2);
        C::Flip();
        if (!C::Enabled()) {
            Require(C::Snapshot().publications == 0 && C::Snapshot().cpuBytes == 0, "disabled census recorded work");
            return 0;
        }
        Require(C::Snapshot().cpuBytes == 100 && C::Snapshot().gpuOnlyBytes == 0, "CPU read after GPU read was lost");
        C::ResetForTest();
        {
            const C::ProducerScope source(C::Producer{32, 1, 0xabc, 0});
            C::Produced(100, 200, 100);
        }
        Require(C::ProducerFor(100, 200) == C::Producer{32, 1, 0xabc, 100}, "producer provenance was lost after leaving its queue scope");
        C::Published(100, 200, C::Kind::CopyBack);
        C::CpuRead(100, 100, 32, 1);
        C::Flip();
        Require(C::Snapshot().untouchedBytes == 100, "a fault predating publication was attributed to the new result");
        C::ResetForTest();
        C::ResetForTest();
        C::Published(100, 200, C::Kind::CopyBack);
        C::Overwritten(120, 180);
        C::CpuRead(120, 60, 2);
        C::GpuRead(100, 120, 0);
        C::GpuRead(180, 200, 1);
        C::Flip();
        Require(C::Snapshot().gpuOnlyBytes == 100 && C::Snapshot().cpuBytes == 0, "partial overwrite removed surviving bytes or retained overwritten bytes");
        C::ResetForTest();
        C::Published(100, 200, C::Kind::CopyBack);
        C::Published(120, 180, C::Kind::ShadowPublish);
        C::CpuRead(120, 60, 2);
        C::CpuRead(200, 1, 2);
        C::CpuRead(std::numeric_limits<std::uint64_t>::max(), 2, 2);
        C::Flip();
        auto totals = C::Snapshot();
        Require(totals.bytes == 160 && totals.cpuBytes == 60 && totals.untouchedBytes == 100, "republication overlap or half-open range accounting failed");
        C::CpuRead(100, 20, 2);
        C::Flip();
        Require(C::Snapshot().cpuBytes == 60, "later-frame reader changed the publication-frame census");
        C::ResetForTest();
        C::Published(100, 200, C::Kind::CopyBack);
        C::Produced(100, 200);
        C::CpuRead(100, 100, 2);
        C::Published(100, 200, C::Kind::BlockPublish);
        C::GpuRead(100, 200, 6);
        C::Flip();
        totals = C::Snapshot();
        Require(totals.bytes == 200 && totals.untouchedBytes == 100 && totals.gpuOnlyBytes == 100, "producer overwrite did not retire the old publication");
        Require(totals.verifyChecks > 0 && totals.verifyMismatches == 0, "interval verifier did not run cleanly");
        for (unsigned i = 0; i < 62; ++i) C::Flip();
        std::cout << "GPU authority census bookkeeping passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
