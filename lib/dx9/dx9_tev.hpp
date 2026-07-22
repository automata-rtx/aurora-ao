#pragma once

#ifdef AURORA_ENABLE_D3D9

#include "dx9_internal.hpp"
#include "dx9_vertex.hpp"

namespace aurora::dx9 {

// Translates the current TEV configuration (g_gxState.tevStages etc.) into
// D3D9 texture-stage states, binds the sampled textures, and configures
// texgen (TEXCOORDINDEX / texture matrices). Fixed-function only; anything
// inexpressible degrades per docs/dx9/gx-to-d3d9-mapping.md #9 with a
// warn_once. Returns the number of D3D stages used.
uint32_t apply_tev(const DecodedDraw& draw) noexcept;

} // namespace aurora::dx9

#endif // AURORA_ENABLE_D3D9
