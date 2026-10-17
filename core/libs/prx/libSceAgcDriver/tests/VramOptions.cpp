#include "prx/libSceAgcDriver/Graphics/include/VramOptions.hpp"
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    const auto expected = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
    try {
        AgcDriver::Graphics::ValidateVramOptions();
        if (expected.empty()) return 0;
        std::cerr << "invalid VRAM option combination was accepted\n";
    } catch (const std::exception& error) {
        if (!expected.empty() && std::string_view(error.what()).find(expected) != std::string_view::npos) return 0;
        std::cerr << error.what() << '\n';
    }
    return 1;
}
