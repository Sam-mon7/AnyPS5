#ifndef AGCDRIVER_GRAPHICS_STREAMCENSUS_HPP
#define AGCDRIVER_GRAPHICS_STREAMCENSUS_HPP

namespace AgcDriver::Graphics {

struct DeviceFunctions;

namespace StreamCensus {

void Install(DeviceFunctions& functions);
void EndFrame();

}

}

#endif
