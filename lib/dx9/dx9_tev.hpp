#pragma once

#ifdef AURORA_ENABLE_D3D9

#include "dx9_internal.hpp"
#include "dx9_vertex.hpp"

namespace aurora::dx9 {

// Translates the current TEV configuration (g_gxState.tevStages etc.) into
// D3D9 texture-stage states, binds the sampled textures, and configures
// texgen (TEXCOORDINDEX / texture matrices). Returns the number of D3D stages
// used. What a fixed-function chain cannot express degrades with a warn_once
// (docs/dx9/gx-to-d3d9-mapping.md #9).
//
// The chain is also the main thing Remix reads a material out of, so the stage
// layout is chosen for that reader rather than for the raster. It is not the
// only route: this fork also reads D3DMATERIAL9 side channels (emissive score,
// ramp endpoint, vertex-colour verdict), so "the stage chain cannot carry it"
// is not a dead end. docs/dx9/remix-material-interface.md §2.
uint32_t apply_tev(const DecodedDraw& draw) noexcept;

} // namespace aurora::dx9

#endif // AURORA_ENABLE_D3D9
