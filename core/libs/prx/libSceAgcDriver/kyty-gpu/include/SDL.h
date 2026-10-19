#ifndef ANYPS5_KYTY_GPU_SDL_H
#define ANYPS5_KYTY_GPU_SDL_H

#include <cstdint>

struct HWND__;

using Uint8 = std::uint8_t;
using Uint32 = std::uint32_t;
using Sint32 = std::int32_t;

enum SDL_bool { SDL_FALSE = 0, SDL_TRUE = 1 };

struct SDL_Window {
    void* hwnd = nullptr;
    int width = 0;
    int height = 0;
};

struct SDL_WindowEvent {
    Uint32 type = 0;
    Uint32 timestamp = 0;
    Uint32 windowID = 0;
    Uint8 event = 0;
    Sint32 data1 = 0;
    Sint32 data2 = 0;
};

struct SDL_DisplayEvent {
    Uint32 type = 0;
    Uint32 timestamp = 0;
    Uint32 display = 0;
    Uint8 event = 0;
    Sint32 data1 = 0;
};

union SDL_Event {
    Uint32 type;
    SDL_WindowEvent window;
    SDL_DisplayEvent display;
};

struct SDL_DisplayMode {
    Uint32 format = 0;
    int w = 0;
    int h = 0;
    int refresh_rate = 0;
    void* driverdata = nullptr;
};

struct SDL_version {
    Uint8 major = 2;
    Uint8 minor = 0;
    Uint8 patch = 0;
};

enum SDL_SYSWM_TYPE { SDL_SYSWM_UNKNOWN = 0, SDL_SYSWM_WINDOWS = 1 };

struct SDL_SysWMinfo {
    SDL_version version;
    SDL_SYSWM_TYPE subsystem = SDL_SYSWM_UNKNOWN;
    union {
        struct {
            HWND__* window;
            void* hdc;
            void* hinstance;
        } win;
    } info{};
};

#define SDL_VERSION(x) static_cast<void>(x)

inline constexpr Uint32 SDL_INIT_VIDEO = 0x20;
inline constexpr Uint32 SDL_INIT_GAMECONTROLLER = 0x2000;

const char* SDL_GetError();
void SDL_DestroyWindow(SDL_Window* window);
void SDL_QuitSubSystem(Uint32 flags);
void SDL_SetWindowTitle(SDL_Window* window, const char* title);
const char* SDL_GetWindowTitle(SDL_Window* window);
SDL_bool SDL_GetWindowWMInfo(SDL_Window* window, SDL_SysWMinfo* info);
void SDL_PumpEvents();
int SDL_GetWindowDisplayIndex(SDL_Window* window);
int SDL_GetCurrentDisplayMode(int display, SDL_DisplayMode* mode);

#endif
