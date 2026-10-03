#pragma once

#include "Controls.h"
#include "Lens.h"

/**
    The card on the CPU, with no GL: one frame's parameters made into the
    lens setup, the setup made into the numbers the shader is handed, and the
    shader's per-pixel stage transcribed into C++.

    Three things use it, and the point of the file is that they use the SAME
    code:

    - **The FFGL plugin** (`Lenticular.cpp`): `SetupFor` and `UniformsFor`
      are what `ProcessOpenGL` hands the GPU. The arithmetic used to live
      inline there; it lives here so the CPU twin below is handed the same
      floats bit for bit, not a second transcription of them.
    - **The OpenFX plugin** (`ofx/LenticularOFX.cpp`): the same two, then
      `Shade` for every output pixel -- and, at the two ends of a transition,
      `CardStrength` and `Plain` (OpenFX only, below). That file does
      marshalling only.
    - **`lntest --cpu`**: `Shade` against the real FFGL plugin's GPU render of
      the same two inputs, per pixel, at several fader positions and
      settings, with negative controls.

    ---------------------------------------------------------- the mirrored stage

    Everything below marked `//= mirrored` has a twin of the same name in
    `kLenticularShader` (Shaders.cpp), which is marked the same way, and is a
    line-for-line transcription of it in float: the same operations in the
    same order, so the two disagree only where a GPU's float differs from
    IEEE (division, `atan`, `sin`, `exp`, texture filtering weights). **Change
    one, change both, and run `lntest --cpu`** -- it fails if they drift.

    The sampling is part of the mirror. The GPU fetches each input through
    GL_LINEAR with CLAMP_TO_EDGE and no mipmaps, at a position clamped half a
    texel inside the picture, so `Sample` is exactly that: the bilinear
    weights of the GL specification's equation for a level-0 LINEAR fetch,
    texel centres at ( i + 1/2 ) / W. The moire bands and the ghost both
    depend on it -- a lens samples the print once, between texels, and a
    nearest-texel fetch would move every one of those samples by up to half a
    texel. `lntest --cpu` carries that as a negative control.

    ------------------------------------------------------------- what is not

    Not mirrored: the harness's GPU faults (`Fault` in Shaders.h). They exist
    to show the GPU checks can fail and have no meaning to a host.
    `Uniforms::fault` here carries only `kFaultNearestTexel`, the CPU twin's
    own negative control, and is 0 everywhere but `lntest --cpu`.
*/
namespace lenticular::card
{

/// The parameters as a host holds them, 0..1 (Squeeze 0 or 1). FFGL's are
/// floats; OpenFX's are doubles, cast to float on the way in so that the
/// same slider positions are the same floats in both builds -- which is what
/// makes matched pitches exactly matched there too.
///
/// The defaults ARE the plugin's defaults: the FFGL constructor and the
/// OpenFX descriptor both read them from here.
struct HostValues
{
	float squeeze         = 1.0f;
	float interleavePitch = ParamForPitch( kPitchDefault );
	float bleed           = ParamForBleed( 0.1 );
	float lensPitch       = ParamForPitch( kPitchDefault );
	float focalLength     = ParamForFocal( kFocalDefault );
	float focusSpot       = ParamForSpot( 0.1 );
	float distance        = ParamForDistance( 5.0 );
	float opacity         = 1.0f;///< the tilt: FFGL's Opacity, OpenFX's Transition
	float angleRange      = static_cast< float >( kAngleRangeDefaultDeg / kAngleRangeMaxDeg );
	float ridgeShine      = 0.3f;
	float lightAngle      = static_cast< float >( 0.5 + 0.5 * kLightDefaultDeg / kLightMaxDeg );
};

/// What one frame's parameters make of the card, in double. See Lens.h.
lens::Setup SetupFor( const HostValues& values );

/// The numbers the fragment shader is handed, as the floats it gets. One per
/// uniform of kLenticularShader except the per-input ones (MaxUV, HalfTexel),
/// which belong to `Texture`.
struct Uniforms
{
	int outW = 0;
	int outH = 0;

	float lensPerWidth  = 0.0f;
	float printPerWidth = 0.0f;
	float pitchRatio    = 0.0f;
	float focal         = 0.0f;
	float spotPeriods   = 0.0f;
	float bleedPeriods  = 0.0f;
	int squeeze         = 1;

	float sinTilt     = 0.0f;
	float cosTilt     = 1.0f;
	float invDistance = 0.0f;

	float shine          = 0.0f;
	float lightAngle     = 0.0f;
	float distort        = 0.0f;
	float ridgeSlope     = 0.0f;
	float highlightWidth = 0.0f;
	float edgeShade      = 0.0f;

	/// 0 except in lntest's negative control. See the file comment.
	int fault = 0;
};

/// The uniforms for this setup on a W x H output.
Uniforms UniformsFor( const lens::Setup& setup, int outW, int outH );

/// One input as the GPU sees a texture: RGBA float, row 0 at the BOTTOM (GL's
/// orientation and OpenFX's), the values exactly as stored -- the shader does
/// no alpha arithmetic on a fetch, so neither does this. Not owned.
///
/// No padding: a host buffer is exactly its picture, so MaxUV is 1 and the
/// half-texel inset is all that is left of the FFGL fetch's per-input terms.
struct Texture
{
	const float* rgba = nullptr;
	int width         = 0;
	int height        = 0;
	float halfTexelX  = 0.0f;///< 0.5 / width, as the FFGL side computes it
	float halfTexelY  = 0.0f;
};

Texture MakeTexture( const float* rgba, int width, int height );

/// A level-0 GL_LINEAR, CLAMP_TO_EDGE fetch at texture coordinate (s, t).
void Sample( const Texture& texture, float s, float t, float out[ 4 ], int fault = 0 );

/// The fragment shader's main for output pixel (px, py), counted from the
/// bottom-left: A is the picture printed in each lens's first strip (FFGL's
/// layer below, OpenFX's SourceFrom), B in the second (this layer,
/// SourceTo). Writes RGBA.
///
/// A pure function of its arguments: no clock, no state, nothing from any
/// other frame. Safe to call from any number of threads at once.
void Shade( const Uniforms& uniforms, const Texture& a, const Texture& b, int px, int py, float out[ 4 ] );

/// `Shade` over output rows [ rowBegin, rowEnd ), into `out`, which is
/// outW x outH RGBA float, row 0 at the bottom.
void Render( const Uniforms& uniforms, const Texture& a, const Texture& b, float* out, int rowBegin, int rowEnd );

//---------------------------------------------------------------------------
// The ends: OpenFX only, and NOT mirrored -- the FFGL build has no ends and
// no GLSL twin of any of this.
//
// A Resolume transition can pop from the plain clip to the card and back;
// an NLE transition that did would read as a glitch on the timeline. So the
// OpenFX build fades the card in over the first End Length of the transition
// and out over the last (Ends = Fade, the default): a crossfade, in
// premultiplied colour, between the plain picture -- SourceFrom in the first
// half, SourceTo in the second -- and the card, whose own tilt runs
// throughout exactly as it does without the fade. Ends = Cut is the card
// alone at every Transition value, which is the FFGL behaviour and the
// OpenFX build's output before the ends existed, bit for bit.
//---------------------------------------------------------------------------

/// End Length: the fraction of the transition each end ramp lasts.
inline constexpr float kEndLengthDefault = 0.15f;
inline constexpr float kEndLengthMax     = 0.5f;

/// How much of the card is seen at this Transition value, 0..1. Cut: 1
/// always. Fade: smoothstep over the first and the last `endLength` of the
/// transition -- exactly 0 at Transition 0 and 1 (so the picture there is
/// exactly the plain clip), exactly 1 from `endLength` to 1 - `endLength`,
/// with zero slope at both ends of each ramp, so there is no kink. An End
/// Length of 0 is the card everywhere but at exactly 0 and 1.
float CardStrength( float transition, bool fade, float endLength );

/// The picture with no lens at output pixel (px, py): the input's own texel
/// when the input is the output's size -- exactly, not a filtered fetch that
/// happens to land on it -- and otherwise the clamped GL_LINEAR fetch at the
/// pixel's centre, as an input of another size is stretched over the card.
void Plain( const Uniforms& uniforms, const Texture& texture, int px, int py, float out[ 4 ] );

} // namespace lenticular::card
