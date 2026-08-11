// SDL3 shim for the syntax harness - only what aurora's public headers name.
// Real SDL3 is not needed to type-check the D3D9 backend, and vendoring 23 headers
// to check two typedefs would be worse than saying so.
#pragma once
#include "SDL_stdinc.h"
typedef union SDL_Event {
  uint32_t type;
  uint8_t padding[128];
} SDL_Event;
