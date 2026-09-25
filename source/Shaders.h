#pragma once

/**
    The pass, as GLSL. There is only one.

    A lenticular card is one decision per pixel -- which lens is this column
    under, where does that lens focus on the print for the angle the eye
    sees it at, and what is printed there -- so nothing here needs a buffer
    of its own, and nothing depends on the previous frame. What it does need
    is **two inputs**: `TextureA` is the layer below (Dest,
    `inputTextures[0]`) and `TextureB` is this layer (Src,
    `inputTextures[1]`). Opacity 0 tilts the card to A, 1 to B.

    The two can be **different resolutions**, and each therefore has its own
    `MaxUV` and its own half-texel inset, applied once in its own fetch. The
    vertex shader passes UV through unscaled, exactly as genlock, wipe and
    relay do and for the same reason: every fetch is at a position the lens
    chose, in PICTURE space, and a position in one input's texture space is
    a different place in the other's.

    The pixel's column is `floor( uv * OutSize )`, an integer, and every
    position after that is computed from it -- not from the interpolated
    `uv`, which the GL promises only to about 1 part in 10^5 (wipe's CI
    lesson). Rounding an interpolated coordinate that is half a pixel from
    either integer to an integer is exact on any conforming rasteriser for
    any width under 50,000 pixels.
*/

namespace lenticular
{

extern const char* const kVertexShader;
extern const char* const kLenticularShader;

/// The negative controls' bitmask, shared with the CPU side. The shipped
/// plugin always carries 0; only the harness sets any of these.
enum Fault : int
{
	kFaultNone         = 0,
	kFaultFoldedMaxUV  = 1 << 0,///< A's MaxUV folded into the vertex shader (the genlock trap) -- --mixer
	kFaultSinFocal     = 1 << 1,///< the focal point at F sin t, not F tan t -- --flip
	kFaultFlatDistance = 1 << 2,///< the viewer at infinity whatever Distance says -- --distance
	kFaultPitchLocked  = 1 << 3,///< the lens pitch forced to the print's -- --moire
	kFaultTiltReversed = 1 << 4,///< Opacity tilts the card the other way -- --opacity
};

} // namespace lenticular
