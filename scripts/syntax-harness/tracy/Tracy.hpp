// Tracy profiler shim for the syntax harness. The real Tracy macros expand to RAII
// scope objects; for a syntax check they only have to disappear without changing the
// surrounding statement's shape.
#pragma once
#define ZoneScoped
#define ZoneScopedN(name)
#define ZoneScopedS(depth)
#define ZoneText(txt, size)
#define ZoneName(txt, size)
#define FrameMark
#define FrameMarkNamed(name)
