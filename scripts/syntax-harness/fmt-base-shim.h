// Copied over the real fmt include tree as fmt/base.h by scripts/check_syntax.sh.
//
// fmt 11 renamed core.h to base.h; the distro package is fmt 9, which still calls it
// core.h. This bridges the name.
//
// FMT_CONSTEVAL is emptied first. fmt 9's compile-time format-string checker does not
// resolve the `format_as` overloads aurora declares for GX enums the way fmt 11 does,
// so it rejects strings the real build accepts. Emptying FMT_CONSTEVAL turns the
// checking constructor into an ordinary one - which means THIS HARNESS DOES NOT
// VALIDATE FORMAT STRINGS. A wrong placeholder count still compiles here and fails in
// CI. Everything else - our own types, signatures, overloads, members - is checked.
#pragma once
#ifndef FMT_CONSTEVAL
#define FMT_CONSTEVAL
#endif
#include "core.h"
