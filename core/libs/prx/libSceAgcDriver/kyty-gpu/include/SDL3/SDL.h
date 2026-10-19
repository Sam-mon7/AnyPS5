#ifndef ANYPS5_KYTY_GPU_SDL3_SDL_H
#define ANYPS5_KYTY_GPU_SDL3_SDL_H

#include <cstdint>

struct SDL_Window {
    void* hwnd = nullptr;
    int width = 0;
    int height = 0;
};

struct SDL_WindowEvent {
    std::uint32_t type = 0;
    std::int32_t data1 = 0;
    std::int32_t data2 = 0;
};

struct SDL_DisplayEvent {
    std::uint32_t type = 0;
};

union SDL_Event {
    std::uint32_t type;
    SDL_WindowEvent window;
    SDL_DisplayEvent display;
};

using SDL_MainThreadCallback = void (*)(void*);

inline constexpr std::uint32_t SDL_INIT_VIDEO = 0x20;
inline constexpr std::uint32_t SDL_INIT_GAMEPAD = 0x2000;

const char* SDL_GetError();
void SDL_DestroyWindow(SDL_Window* window);
void SDL_QuitSubSystem(std::uint32_t flags);
bool SDL_RunOnMainThread(SDL_MainThreadCallback callback, void* userdata, bool wait_complete);
bool SDL_GetWindowSizeInPixels(SDL_Window* window, int* width, int* height);

#endif
