// SDL3 shim for the syntax harness. See SDL_events.h.
#pragma once
#include "SDL_stdinc.h"
#include "SDL_events.h"
#include "SDL_joystick.h"
struct SDL_Window;
struct SDL_Renderer;
#define SDL_PROP_WINDOW_WIN32_HWND_POINTER "SDL.window.win32.hwnd"
#define SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER "SDL.window.win32.instance"

extern "C" {
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window* window);
void* SDL_GetPointerProperty(SDL_PropertiesID props, const char* name, void* default_value);
}
