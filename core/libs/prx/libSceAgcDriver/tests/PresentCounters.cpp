#include "prx/libSceAgcDriver/Execution/include/PresentCounterBook.hpp"
#include <cstdio>
#include <stdexcept>

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

}

int main() {
    try {
        AgcDriver::PresentCounterBook book;
        const auto before = book.Snapshot();
        book.Attempt();
        book.Acquire(VK_NOT_READY);
        book.Attempt();
        book.Acquire(VK_ERROR_OUT_OF_DATE_KHR);
        book.Attempt();
        book.Acquire(VK_SUCCESS);
        book.Present(VK_SUCCESS);
        const auto after = book.Snapshot();
        check(after.attempts - before.attempts == 3, "attempts must advance by three");
        check(after.presented - before.presented == 1, "only the queued success may count as presented");
        check(after.acquireDropped - before.acquireDropped == 1, "VK_NOT_READY must count as an acquire drop");
        check(after.outOfDate - before.outOfDate == 1, "out-of-date acquire must be counted separately");
        book.Attempt();
        book.Acquire(VK_TIMEOUT);
        book.Attempt();
        book.Acquire(VK_SUBOPTIMAL_KHR);
        book.Present(VK_SUBOPTIMAL_KHR);
        book.Attempt();
        book.Present(VK_ERROR_OUT_OF_DATE_KHR);
        book.Attempt();
        book.Present(VK_ERROR_DEVICE_LOST);
        const auto final = book.Snapshot();
        check(final.attempts == 7 && final.presented == 2, "suboptimal counts, failed queues and skipped presents do not");
        check(final.acquireDropped == 2 && final.outOfDate == 1, "timeout counts and queue errors do not change acquire reasons");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    std::puts("Present counter accounting passed");
    return 0;
}
