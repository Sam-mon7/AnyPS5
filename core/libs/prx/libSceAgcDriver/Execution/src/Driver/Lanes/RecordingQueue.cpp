#include "prx/libSceAgcDriver/Execution/include/Driver/Lanes/VulkanQueues.hpp"

namespace AgcDriver::DriverDetail::Lanes {

std::uint32_t& RecordingQueue() {
    struct RecordingQueueStorage {};
    return HostThreadLocal<std::uint32_t, RecordingQueueStorage>();
}

}
