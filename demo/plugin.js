/**
 * Lenticular (LN01) — the browser demo.
 *
 * Lenticular is an FFGL **mixer**: Resolume hands it two textures, the layer
 * below (A, `inputTextures[0]`, printed in each lens's first strip) and this
 * layer (B, `inputTextures[1]`, in the second), and the layer's opacity fader
 * as the tilt of the card. It is the fleet's fourth mixer, and the
 * arrangement is wipe's and relay's: the kit hands a demo one input, so A is
 * the kit's clip (its "Clip" dropdown, relabelled "Clip A", and the only one
 * "Use my own…" replaces) and B is a second generated clip from the kit's own
 * `sources.js`, picked from the transport's one extra dropdown. Both are
 * functions of (uv, time) and neither is footage.
 *
 * What runs for real, and what is a port:
 *
 *   - **The shaders are the plugin's**: `kVertexShader` and
 *     `kLenticularShader` from source/Shaders.cpp, copied across unedited by
 *     script. `demo/tools/check_shaders.py` compares them character for
 *     character and `tools/verify.sh` runs it. Every uniform
 *     `Lenticular::ProcessOpenGL` sets is set here.
 *   - **The CPU half is a hand port, and a small one**: Controls.cpp (every
 *     `...FromParam` and `ParamFor...`), Lens.h's constants and the
 *     arithmetic of ProcessOpenGL that turns the parameters into a
 *     `lens::Setup` and the uniforms. The plugin is stateless — no clock, no
 *     buffer, nothing carried between frames — so there is no frame logic to
 *     port. The port was checked against `lntest --pipe` at a set of
 *     settings (demo/README.md), which is not the same as a reader checking
 *     every line.
 *   - **Everything else is not the plugin**: no Resolume, no layer stack, no
 *     FFGL, GLSL ES 3.00 rather than 4.1 core, no padded textures (both
 *     MaxUVs are 1).
 *
 * Opacity is a slider on this page, in the View group where the plugin
 * declares it. In Resolume a mixer parameter named Opacity is bound to the
 * LAYER's opacity fader (measured on genlock, wipe and relay in Arena 7.27.1;
 * Lenticular itself has not been loaded into Arena yet). Squeeze (index 0) is
 * shown here; Arena hides a mixer's first parameter. "Rock the card" in the
 * transport is the page's, not the plugin's: it moves the Opacity slider the
 * way a hand on the layer fader would. The page says all of this in its
 * disclosure.
 */

import { mountDemo } from './vendor/demo.js';
import { Program } from './vendor/gl.js';
import { SOURCES, SourceRenderer } from './vendor/sources.js';

//---------------------------------------------------------------------------
// The plugin's GLSL, from source/Shaders.cpp. DO NOT EDIT HERE: change the
// C++ and copy it across; demo/tools/check_shaders.py fails verify otherwise.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

uniform vec2 MaxUVA;
uniform int Fault;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = ( ( Fault & 1 ) != 0 ) ? vUV * MaxUVA : vUV;
}
`;

const LENTICULAR = `#version 410 core

//inputTextures[0] -- Dest, the layer BELOW: A, printed in the first strip.
uniform sampler2D TextureA;
//inputTextures[1] -- Src, THIS layer: B, printed in the second strip.
uniform sampler2D TextureB;

//Each input has its own. They are not the same number and there is no
//circumstance in which using one for the other is safe.
uniform vec2 MaxUVA;
uniform vec2 MaxUVB;
uniform vec2 HalfTexelA;
uniform vec2 HalfTexelB;
uniform ivec2 OutSize;   //the host frame, W x H

//The sheet and the print, per picture width.
uniform float LensPerWidth;  //N_L
uniform float PrintPerWidth; //N_P
uniform float PitchRatio;    //N_P / N_L, print periods per lens
uniform float Focal;         //F, in lens pitches
uniform float SpotPeriods;   //the focus spot's width on the print, in periods
uniform float BleedPeriods;  //the ramp between two strips, in periods
uniform int Squeeze;         //1: each strip holds its picture squeezed, as a real interleave

//The viewer: tilt T of the card, and 1 / distance in picture widths (0 is
//infinity).
uniform float SinTilt;
uniform float CosTilt;
uniform float InvDistance;

//The surface. Shine 0 is an ideal lens and none of it runs.
uniform float Shine;
uniform float LightAngle;     //radians from the normal
uniform float Distort;        //mu, the residual magnification across a lens
uniform float RidgeSlope;     //sin of the lens edge's slope
uniform float HighlightWidth; //in lens pitches
uniform float EdgeShade;

//The harness's negative controls. 0 in the shipped plugin.
uniform int Fault;

in vec2 uv;
out vec4 fragColor;

const int kFaultFoldedMaxUV  = 1;
const int kFaultSinFocal     = 2;
const int kFaultFlatDistance = 4;

//Clamped half a texel inside the used area: GL_LINEAR at the boundary takes
//half its weight from the texture's undrawn padding.
vec4 fetchA( vec2 p )
{
	if( ( Fault & kFaultFoldedMaxUV ) != 0 )
		return texture( TextureA, p );
	vec2 q = clamp( p, HalfTexelA, vec2( 1.0 ) - HalfTexelA );
	return texture( TextureA, q * MaxUVA );
}

vec4 fetchB( vec2 p )
{
	if( ( Fault & kFaultFoldedMaxUV ) != 0 )
		return texture( TextureB, p );
	vec2 q = clamp( p, HalfTexelB, vec2( 1.0 ) - HalfTexelB );
	return texture( TextureB, q * MaxUVB );
}

//The print's B fraction at a point: 0 on A's strip, 1 on B's, with a linear
//ramp BleedPeriods wide centred on each strip edge. u is the phase in its
//period, [0, 1): B -> A at 0, A -> B at 0.5.
float stepUp( float x )
{
	if( BleedPeriods <= 0.0 )
		return x >= 0.0 ? 1.0 : 0.0;
	return clamp( 0.5 + x / BleedPeriods, 0.0, 1.0 );
}

float stripB( float u )
{
	return 1.0 - stepUp( u ) + stepUp( u - 0.5 ) - stepUp( u - 1.0 );
}

//The antiderivative of stepUp, zero far to the left.
float rampIntegral( float x )
{
	float h = 0.5 * BleedPeriods;
	if( x <= -h )
		return 0.0;
	if( x >= h )
		return x;
	return ( x + h ) * ( x + h ) / ( 2.0 * BleedPeriods );
}

//The integral of stripB from 0 to x >= 0: half a period of B in every whole
//period, and the part period in closed form.
float integralB( float x )
{
	float n = floor( x );
	float u = x - n;
	float part = u - ( rampIntegral( u ) - rampIntegral( 0.0 ) ) + rampIntegral( u - 0.5 ) - rampIntegral( u - 1.0 );
	return 0.5 * n + part;
}

//The mean of the print's B fraction across the focus spot centred on this
//phase. A spot wholly on one strip's flat part is exactly 0 or 1 -- a
//branch, not a cancellation -- so a card tilted onto one strip returns that
//picture's texel unchanged.
float coverageB( float phase )
{
	float w  = SpotPeriods;
	float lo = phase - 0.5 * w;
	float n  = floor( lo );
	float a  = lo - n;
	float b  = a + w;
	float h  = 0.5 * BleedPeriods;
	if( b <= 1.0 )
	{
		if( a >= 0.5 + h && b <= 1.0 - h )
			return 1.0;
		if( a >= h && b <= 0.5 - h )
			return 0.0;
	}
	if( w <= 1.0e-5 )
		return stripB( fract( phase ) );
	return ( integralB( b ) - integralB( a ) ) / w;
}

void main()
{
	//The pixel, as integers, and its centre as a fraction of the picture.
	vec2 px = clamp( floor( uv * vec2( OutSize ) ), vec2( 0.0 ), vec2( OutSize ) - vec2( 1.0 ) );
	float X = ( px.x + 0.5 ) / float( OutSize.x );
	float Y = ( px.y + 0.5 ) / float( OutSize.y );

	//The lens this column is under, and where across it.
	float xl = X * LensPerWidth;
	float k  = floor( xl );
	float e  = xl - k - 0.5;

	//The angle this column is seen at: the card's tilt, less the column's
	//offset from the centre over the viewer's distance.
	float across  = ( ( Fault & kFaultFlatDistance ) != 0 ) ? 0.0 : ( X - 0.5 ) * InvDistance;
	float tanView = ( SinTilt - across ) / CosTilt;
	float reach   = tanView;
	if( ( Fault & kFaultSinFocal ) != 0 )
		reach = tanView * inversesqrt( 1.0 + tanView * tanView );

	//Where the lens focuses on the print, and that point's phase in the
	//interleave: ( k + 0.5 + F tan t + mu e ) N_P / N_L. Taken as a whole
	//number of periods nb plus a lead under two periods, so the lens index
	//never rounds into the fraction: with matched pitches the lead is the
	//same float under every lens, and the whole card flips on one number.
	float base = ( k + 0.5 ) * PitchRatio;
	float nb   = floor( base );
	float lead = ( base - nb ) + ( Focal * reach + Distort * e ) * PitchRatio;
	float nl   = floor( lead );
	float u    = lead - nl;
	float n    = nb + nl;

	//What is printed there. Squeezed: the strip holds its picture's band of
	//one period in half a period. Not: the strip is a window on the picture
	//at its own position.
	float posA = n + u;
	float posB = n + u;
	if( Squeeze != 0 )
	{
		posA = n + min( 2.0 * u, 1.0 );
		posB = n + max( 2.0 * u - 1.0, 0.0 );
	}

	//The pattern is periodic, so the spot is integrated about the lead.
	float m = coverageB( lead );
	vec4 colour;
	if( m <= 0.0 )
		colour = fetchA( vec2( posA / PrintPerWidth, Y ) );
	else if( m >= 1.0 )
		colour = fetchB( vec2( posB / PrintPerWidth, Y ) );
	else
		colour = mix( fetchA( vec2( posA / PrintPerWidth, Y ) ), fetchB( vec2( posB / PrintPerWidth, Y ) ), m );

	//The ridge: a highlight where the lens surface's normal bisects the
	//light and the eye, and a little shading towards each lens's edge.
	//Composited OVER, as a white of coverage h, so it lights a transparent
	//picture as the plastic sheet in front of it would.
	if( Shine > 0.0 )
	{
		float view    = atan( tanView );
		float bisect  = 0.5 * ( LightAngle + view );
		float spotAt  = sin( bisect ) / ( 2.0 * RidgeSlope );
		float g       = ( e - spotAt ) / HighlightWidth;
		float h       = clamp( Shine * exp( -g * g ), 0.0, 1.0 );
		colour.rgb   *= 1.0 - EdgeShade * Shine * 4.0 * e * e;
		colour        = vec4( h ) + ( 1.0 - h ) * colour;
	}

	fragColor = colour;
}
`;

//===========================================================================
// Controls.cpp and Lens.h, ported. Every host value arrives as a float, so
// it is rounded to one here (f32) before the double arithmetic the plugin
// does on it, and every uniform is a float of the double result.
//===========================================================================
const f32 = Math.fround;
const clamp01 = (v) => Math.min(1.0, Math.max(0.0, v));
const kPi = 3.14159265358979323846;
const kDegree = kPi / 180.0;

// Controls.h
const kPitchMin = 4.0;
const kPitchMax = 480.0;
const kPitchDefault = 60.0;
const kFocalMin = 0.5;
const kFocalMax = 6.0;
const kFocalDefault = 1.8;
const kSpotMax = 0.5;
const kBleedMax = 1.0;
const kInvDistanceMax = 2.0;
const kAngleRangeMaxDeg = 45.0;
const kAngleRangeDefaultDeg = 8.0;
const kLightMaxDeg = 60.0;
const kLightDefaultDeg = 25.0;

// Lens.h
const kRidgeEdgeSlopeDeg = 35.0;
const kHighlightWidth = 0.06;
const kHighlightMinPx = 0.75;
const kEdgeShade = 0.25;
const kDistortMax = 0.12;
const kSpotPeriodsMax = 4.0;

const geometric = (t, lo, hi) => lo * Math.pow(hi / lo, clamp01(t));
const geometricParam = (value, lo, hi) => (!(value > 0.0) ? 0.0 : clamp01(Math.log(value / lo) / Math.log(hi / lo)));

const PitchFromParam = (v) => geometric(v, kPitchMin, kPitchMax);
const FocalFromParam = (v) => geometric(v, kFocalMin, kFocalMax);
const SpotFromParam = (v) => clamp01(v) * kSpotMax;
const BleedFromParam = (v) => clamp01(v) * kBleedMax;
// 1.0f gives exactly 0: infinity is a value the slider can hold.
const InvDistanceFromParam = (v) => (1.0 - clamp01(v)) * kInvDistanceMax;
const AngleRangeFromParam = (v) => clamp01(v) * kAngleRangeMaxDeg * kDegree;
const TiltFromParams = (opacity, angleRange) => (2.0 * clamp01(opacity) - 1.0) * AngleRangeFromParam(angleRange);
const LightFromParam = (v) => (2.0 * clamp01(v) - 1.0) * kLightMaxDeg * kDegree;

const ParamForPitch = (n) => f32(geometricParam(n, kPitchMin, kPitchMax));
const ParamForFocal = (f) => f32(geometricParam(f, kFocalMin, kFocalMax));
const ParamForSpot = (w) => f32(clamp01(w / kSpotMax));
const ParamForBleed = (b) => f32(clamp01(b / kBleedMax));
const ParamForDistance = (d) => (!(d > 0.0) || !Number.isFinite(d) ? 1.0 : f32(clamp01(1.0 - (1.0 / d) / kInvDistanceMax)));

const deg = (radians) => radians / kDegree;
// The viewing zone either side of the flip: atan( 1 / 2F ).
const zoneDeg = (focal) => deg(Math.atan(1.0 / (2.0 * focal)));

//===========================================================================
// The parameters, in Lenticular.h's ParamID order, with Lenticular::
// Lenticular()'s names, groups, types and defaults. The defaults the
// constructor computes (ParamForPitch( kPitchDefault ) and so on) are
// computed here with the same ported functions, float-rounded as the
// plugin's are.
//===========================================================================
const PARAMS = [
  // Index 0, which Resolume Arena does not show for a mixer: on, for ever.
  { id: 'squeeze', name: 'Squeeze', type: 'boolean', default: 1, group: 'Print',
    hint: 'On: each strip holds its picture squeezed into half a period, as a real interleave does. Off: each strip is a window on the picture at its own position. Index 0: Resolume Arena does not show a mixer’s first parameter (measured on genlock, wipe and relay), so in Arena this stays on. A browser does not hide it.' },
  { id: 'interleavePitch', name: 'Interleave Pitch', type: 'standard', default: ParamForPitch(kPitchDefault), group: 'Print',
    display: (v) => `${PitchFromParam(f32(v)).toFixed(2)} / width`,
    hint: 'How many A-then-B strip pairs are printed across the picture width, 4 to 480, geometric. Shares Lens Pitch’s mapping, so the same slider position is the same pitch exactly — no moiré.' },
  { id: 'bleed', name: 'Bleed', type: 'standard', default: ParamForBleed(0.1), group: 'Print',
    display: (v) => `${(BleedFromParam(f32(v)) * 100).toFixed(1)} %`,
    hint: 'The ramp between two strips, as a fraction of a strip. At 100 % the print is a triangle wave.' },

  { id: 'lensPitch', name: 'Lens Pitch', type: 'standard', default: ParamForPitch(kPitchDefault), group: 'Lens',
    display: (v) => `${PitchFromParam(f32(v)).toFixed(2)} / width`,
    hint: 'How many cylindrical lenses across the picture width, 4 to 480, geometric. Detune it from Interleave Pitch and the flip happens in moiré bands W / |N_L − N_P| pixels apart, which sweep as the card tilts.' },
  { id: 'focalLength', name: 'Focal Length', type: 'standard', default: ParamForFocal(kFocalDefault), group: 'Lens',
    display: (v) => { const F = FocalFromParam(f32(v)); return `${F.toFixed(2)} · ±${zoneDeg(F).toFixed(1)}°`; },
    hint: 'In lens pitches, 0.5 to 6, geometric. The print sits in the focal plane; one strip is seen over ±atan( 1 / 2F ) either side of the flip — 15.5° at the default 1.8.' },
  { id: 'focusSpot', name: 'Focus Spot', type: 'standard', default: ParamForSpot(0.1), group: 'Lens',
    display: (v) => `${SpotFromParam(f32(v)).toFixed(3)} pitch`,
    hint: 'The width of the lens’s focus on the print, 0 to 0.5 lens pitches. Where the spot straddles two strips you see both: the ghost.' },
  { id: 'distance', name: 'Distance', type: 'standard', default: ParamForDistance(5.0), group: 'Lens',
    display: (v) => { const inv = InvDistanceFromParam(f32(v)); return inv > 0 ? `${(1 / inv).toFixed(2)} widths` : '∞'; },
    hint: 'The viewer’s distance from the card, linear in 1 / D from half a picture width (0) to infinity (1). A near viewer sees each column at its own angle, so the flip sweeps across the card at x = D sin T.' },

  { id: 'opacity', name: 'Opacity', type: 'standard', default: 1.0, group: 'View',
    display: (v) => `tilt ${deg(TiltFromParams(f32(v), f32(PAGE.params?.get('angleRange') ?? f32(kAngleRangeDefaultDeg / kAngleRangeMaxDeg)))).toFixed(2)}°`,
    hint: 'The tilt of the card: 0 is −Angle Range (A), 1 is +Angle Range (B), 0.5 exactly square on. In Resolume this is the LAYER’s opacity fader (measured on the fleet’s other mixers); here it is a slider, and “Rock the card” moves it for you.' },
  { id: 'angleRange', name: 'Angle Range', type: 'standard', default: f32(kAngleRangeDefaultDeg / kAngleRangeMaxDeg), group: 'View',
    display: (v) => `±${deg(AngleRangeFromParam(f32(v))).toFixed(2)}°`,
    hint: 'The tilt Opacity 0 and 1 stand for, 0 to 45°. Past the viewing zone the next lens’s strips come round and the card flips back.' },
  { id: 'ridgeShine', name: 'Ridge Shine', type: 'standard', default: f32(0.3), group: 'View',
    display: (v) => clamp01(f32(v)).toFixed(3),
    hint: 'The plastic: a highlight on each lens where its surface bisects the light and the eye, shading towards each lens’s edge, and a slight lens distortion. 0 is an ideal lens.' },
  { id: 'lightAngle', name: 'Light Angle', type: 'standard', default: f32(0.5 + 0.5 * kLightDefaultDeg / kLightMaxDeg), group: 'View',
    display: (v) => `${deg(LightFromParam(f32(v))).toFixed(1)}°`,
    hint: 'Where the light is, −60 to +60° from the card’s normal. Moves the ridge highlight across the lenses.' },
];

// Filled once mounted, for the Opacity display that reads Angle Range.
const PAGE = { params: null };

//===========================================================================
// B, the second input. The kit renders A; this renders B from the same
// generated clips, at the same raster and the same clock.
//===========================================================================
const A_CLIPS = ['scene', 'bars', 'grid', 'ramp', 'spot', 'detail', 'alpha'];
const B_CLIPS = ['bars', 'scene', 'grid', 'ramp', 'spot', 'detail', 'alpha'];
const B_DEFAULT = 'bars';

//===========================================================================
// "Rock the card": the page's, not a plugin control. It moves the Opacity
// slider through its whole range and back every six seconds of the page's
// clock, the way a hand on the layer's fader would, starting from the
// plugin's default of 1. Touching Opacity (or unticking it) stops it.
//===========================================================================
const kRockPeriod = 6.0;
const query = new URLSearchParams(window.location.search);
const embedded = query.has('embed') && query.get('embed') !== '0';
const rock = {
  on: embedded ? query.get('rock') === '1' : query.get('rock') !== '0',
  setting: false,
};

// The harness-side hooks a headless check uses: afterRender( gl, a, b ) runs
// after each draw, with both input textures still alive.
const hooks = { afterRender: null };

const stats = { text: '' };

//===========================================================================
// Lenticular::ProcessOpenGL, as far as a browser has it.
//===========================================================================
function createRenderer(gl, quad) {
  const program = new Program(gl, VERTEX, LENTICULAR, 'lenticular');
  const clipB = new SourceRenderer(gl, quad);
  const loc = (name) => program.location(name);

  return {
    render({ input, params, width, height, time, variant }) {
      if (rock.on && PAGE.state?.playing !== false) {
        rock.setting = true;
        params.set('opacity', 0.5 + 0.5 * Math.cos((2.0 * kPi * time) / kRockPeriod));
        rock.setting = false;
      }

      const P = (id) => f32(params.get(id));
      const outW = width;
      const outH = height;

      //-----------------------------------------------------------------
      // The card, as numbers. In double here; the shader gets floats.
      //-----------------------------------------------------------------
      const s = {};
      s.squeeze = P('squeeze') > 0.5;
      s.printPerWidth = PitchFromParam(P('interleavePitch'));
      s.lensPerWidth = PitchFromParam(P('lensPitch'));
      // Exactly 1.0 when the two sliders agree.
      s.ratio = s.printPerWidth / s.lensPerWidth;
      s.focal = FocalFromParam(P('focalLength'));
      s.spotLens = SpotFromParam(P('focusSpot'));
      s.spotPeriods = Math.min(s.spotLens * s.ratio, kSpotPeriodsMax);
      s.bleedPeriods = 0.5 * BleedFromParam(P('bleed')); // a strip is half a period
      s.invDistance = InvDistanceFromParam(P('distance'));
      s.tilt = TiltFromParams(P('opacity'), P('angleRange'));
      s.shine = Math.min(1.0, Math.max(0.0, P('ridgeShine')));
      s.light = LightFromParam(P('lightAngle'));
      s.distort = kDistortMax * s.shine;

      const highlightWidth = Math.max(kHighlightWidth, (kHighlightMinPx * s.lensPerWidth) / outW);

      //-----------------------------------------------------------------
      // The line under the picture. Not the plugin: it reads the numbers
      // the port chose for this frame.
      //-----------------------------------------------------------------
      const parts = [];
      const tiltDeg = deg(s.tilt);
      parts.push(`tilt ${tiltDeg >= 0 ? '+' : ''}${tiltDeg.toFixed(2)}°${rock.on ? ' (rocking)' : ''}`);
      parts.push(`${s.lensPerWidth.toFixed(2)} lenses, ${(outW / s.lensPerWidth).toFixed(1)} px each`);
      parts.push(`zone ±${zoneDeg(s.focal).toFixed(1)}° at F ${s.focal.toFixed(2)}`);
      const mismatch = Math.abs(s.lensPerWidth - s.printPerWidth);
      parts.push(mismatch > 0 ? `moiré bands every ${(outW / mismatch).toFixed(1)} px` : 'pitches matched: no moiré');
      if (s.invDistance > 0) {
        const D = 1.0 / s.invDistance;
        parts.push(`viewer ${D.toFixed(2)} widths away: square-on flip at x = D sin T = ${(D * Math.sin(s.tilt)).toFixed(3)} widths from the centre`);
      } else {
        parts.push('viewer at infinity: the whole card flips at once');
      }
      if (!s.squeeze) parts.push('Squeeze off');
      stats.text = parts.join(' · ');

      //-----------------------------------------------------------------
      // The inputs and the draw. Every uniform Lenticular::ProcessOpenGL
      // sets.
      //-----------------------------------------------------------------
      const bClip = SOURCES.find((x) => x.id === (variant ?? B_DEFAULT)) ?? SOURCES[0];
      const b = clipB.render(bClip, width, height, time);

      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      gl.disable(gl.BLEND);

      program.use();
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, input.texture);
      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, b.texture);
      program.setSampler('TextureA', 0);
      program.setSampler('TextureB', 1);

      // Unpadded textures in a browser: both MaxUVs are exactly 1.
      program.set('MaxUVA', 1.0, 1.0);
      program.set('MaxUVB', 1.0, 1.0);
      program.set('HalfTexelA', f32(0.5 / input.width), f32(0.5 / input.height));
      program.set('HalfTexelB', f32(0.5 / b.width), f32(0.5 / b.height));
      gl.uniform2i(loc('OutSize'), outW, outH);

      program.set('LensPerWidth', f32(s.lensPerWidth));
      program.set('PrintPerWidth', f32(s.printPerWidth));
      program.set('PitchRatio', f32(s.ratio));
      program.set('Focal', f32(s.focal));
      program.set('SpotPeriods', f32(s.spotPeriods));
      program.set('BleedPeriods', f32(s.bleedPeriods));
      program.setInt('Squeeze', s.squeeze ? 1 : 0);

      program.set('SinTilt', f32(Math.sin(s.tilt)));
      program.set('CosTilt', f32(Math.cos(s.tilt)));
      program.set('InvDistance', f32(s.invDistance));

      program.set('Shine', f32(s.shine));
      program.set('LightAngle', f32(s.light));
      program.set('Distort', f32(s.distort));
      program.set('RidgeSlope', f32(Math.sin(kRidgeEdgeSlopeDeg * kDegree)));
      program.set('HighlightWidth', f32(highlightWidth));
      program.set('EdgeShade', f32(kEdgeShade));

      // The harness's negative controls: 0 in the shipped plugin, 0 here.
      program.setInt('Fault', 0);

      quad.draw();

      if (hooks.afterRender) hooks.afterRender(gl, input, b);

      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, null);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, null);
    },
  };
}

//===========================================================================
// The page.
//===========================================================================
const mounted = mountDemo({
  name: 'Lenticular',
  // The FFGL type the plugin registers (PluginInfo), for the kit banner's
  // closing sentence.
  kind: 'mixer',
  pluginId: 'LN01',
  tagline:
    'A printed lenticular sheet that shows one layer or the other, as an FFGL mixer. The two pictures are cut into strips and interleaved under a sheet of cylindrical lenses, and the layer’s opacity tilts the card: each lens focuses on the strip the viewing angle picks, so the card flips, ghosts where the focus straddles both strips, bands with moiré when the print’s pitch misses the lens’s, sweeps across when the viewer is close, and catches the light on its ridges.',
  repo: 'https://github.com/stoatworks-labs/lenticular',
  page: 'https://stoatworks-labs.com/software/lenticular/',

  blurb:
    'It is Lenticular’s own GLSL, ported from the repository to WebGL2, with the plugin’s parameter conversions ported to JavaScript. Lenticular is a mixer, so it needs two pictures: A (the layer below) is the Clip A dropdown and B (this layer) is Clip B, both generated in this page. In Resolume the layer’s opacity fader tilts the card; here it is the Opacity slider, which “Rock the card” moves for you.',

  sources: A_CLIPS,

  // The kit's one extra transport dropdown. B is transport, not a parameter
  // the plugin declares, which is exactly why it must not be in the
  // inspector.
  variants: {
    label: 'Clip B',
    default: B_DEFAULT,
    options: B_CLIPS.map((id) => {
      const x = SOURCES.find((y) => y.id === id);
      return { id, name: x.name, hint: `B, this layer: ${x.hint}` };
    }),
  },

  params: PARAMS,

  // The plugin declares no presets (AGENTS.md). These are the page's:
  // combinations of the plugin's own parameters and nothing else, each
  // putting one part of the card in front of a visitor.
  presets: {
    'Moiré: lens pitch detuned': { lensPitch: 0.58 },
    'Near viewer: the flip sweeps': { distance: 0.15, angleRange: 0.35 },
    'Soft focus: the ghost': { focusSpot: 0.9, bleed: 0.5 },
    'Fat ridges, bright plastic': { lensPitch: 0.15, interleavePitch: 0.15, ridgeShine: 1.0 },
    'Wide tilt: past the zone': { angleRange: 1.0 },
    'Ideal lens': { focusSpot: 0, bleed: 0, ridgeShine: 0, distance: 1 },
    'Unsqueezed print': { squeeze: 0 },
  },

  differences: [
    'Two inputs, both generated here. The kit this page is built on hands a demo one input; Lenticular is a mixer, so B is rendered by a second copy of the kit’s own clip generator at the same raster and on the same clock (wipe’s and relay’s arrangement). They are synthetic test clips, not Resolume’s demo footage. “Use my own…” replaces A only.',
    'Opacity is a slider. In Resolume Arena a mixer parameter named Opacity is bound to the layer’s opacity fader, and a layer transition ramps it — measured on the fleet’s other mixers (genlock, wipe, relay) in Arena 7.27.1. Lenticular itself has not been loaded into Resolume yet, so that is the expectation, not a measurement of this plugin.',
    'Squeeze is parameter 0. Arena hides a mixer’s first parameter (three mixers for three), which is why Squeeze is first and defaults to on — in Arena it will stay on. Here it is shown and can be turned off, because a browser does not hide it.',
    '“Rock the card” is the page’s, not a plugin control. It moves the Opacity slider from 1 to 0 and back every six seconds of the page’s clock, as a hand on the layer fader would; touching Opacity or unticking it stops it. The plugin has no clock at all: in Resolume the card moves only when the fader does.',
    'The CPU half is a port. Controls.cpp (every conversion from the host’s 0..1), Lens.h’s constants and the arithmetic in ProcessOpenGL that turns the parameters into the card’s uniforms are translated to JavaScript by hand in demo/plugin.js. It was compared with the plugin’s own harness (lntest --pipe) on identical inputs at a handful of settings; outside those, only a reader checks it. The shaders themselves are checked: demo/tools/check_shaders.py fails the repository’s verify script if a character drifts from source/Shaders.cpp.',
    'Both MaxUVs are 1. Resolume hands a mixer two textures that are padded and usually of different sizes, and the shader applies each input’s own MaxUV — the arrangement the plugin’s --mixer check exists for. The page’s textures are unpadded, so that half of the shader runs with nothing to correct. A file of your own does arrive at its own size, so A’s half-texel clamp differs from B’s then.',
    'Output alpha is the seen picture’s — A’s and B’s alpha mixed by the B fraction, with the ridge highlight composited over — as the plugin decides. The canvas here is drawn over black and does not show alpha, so the “Shape on transparency” clip’s transparency is not visible as such.',
    'GLSL ES 3.00 in WebGL2, not desktop GL 4.1 core. The shader text is the plugin’s; only the version line and precision qualifiers are changed by the kit. A different GPU and driver can round differently in the last bit, which at a strip edge can pick the other strip for a pixel.',
    'The line under the picture is not the plugin. It reports what the port computed for this frame — the tilt, the lens count, the viewing zone, the moiré period, where a near viewer’s flip lands. The plugin draws no such thing.',
    'The presets are the page’s. The plugin declares none; each is only a combination of the plugin’s own parameters.',
    'The About block — a text line and link buttons for the host’s panel — is absent: a web page has links of its own. The harness’s negative controls (the Fault uniform) are 0 here as in the shipped plugin. Lenticular has no audio path, so nothing here is missing for want of one.',
    'Nothing here is measured. The plugin’s numerical proof — the card tilted all the way is A or B sampled at the lens pitch bitwise, the flip lands at ±atan( 1 / 2F ), a pitch mismatch bands at the predicted period by FFT, a near viewer’s flip lands at D sin T, one mutated character of GLSL is caught — is tools/lntest in the repository, and that harness, not this page, is the reason to believe the geometry.',
  ],

  createRenderer,
});

PAGE.params = mounted?.params ?? null;
PAGE.state = mounted?.state ?? null;

//---------------------------------------------------------------------------
// The kit labels its clip picker "Clip". With two inputs that is ambiguous,
// so it is renamed for what it feeds, A, the layer below, and moved in front
// of Clip B. `?clipb=<id>` picks B, the way the kit's `?clip=` picks A.
//---------------------------------------------------------------------------
for (const label of document.querySelectorAll('.transport__label')) {
  if (label.textContent === 'Clip') {
    label.textContent = 'Clip A';
    label.title = 'A, the layer below: printed in each lens’s first strip.';
    const fieldA = label.closest('.transport__field');
    const fieldB = [...document.querySelectorAll('.transport__label')]
      .find((l) => l.textContent === 'Clip B')?.closest('.transport__field');
    if (fieldA && fieldB) fieldB.before(fieldA);
  }
}
const wantedB = query.get('clipb');
if (mounted?.state && wantedB && B_CLIPS.includes(wantedB)) {
  mounted.state.variant = wantedB;
  const selectB = [...document.querySelectorAll('.transport__label')]
    .find((l) => l.textContent === 'Clip B')?.closest('.transport__field')?.querySelector('select');
  if (selectB) selectB.value = wantedB;
}

//---------------------------------------------------------------------------
// "Rock the card": transport, never the inspector, because it is the page's
// and not a parameter the plugin declares.
//---------------------------------------------------------------------------
const transport = document.querySelector('.transport');
let rockBox = null;
if (transport && mounted?.params) {
  const rockLabel = document.createElement('label');
  rockLabel.className = 'transport__field';
  rockLabel.title = 'The page’s convenience, not a plugin control: move the Opacity slider 1 → 0 → 1 every six seconds, as a hand on the layer fader would. Touching Opacity stops it.';
  rockBox = document.createElement('input');
  rockBox.type = 'checkbox';
  rockBox.id = 'rock-the-card';
  rockBox.checked = rock.on;
  rockBox.addEventListener('change', () => { rock.on = rockBox.checked; mounted.redraw(); });
  const text = document.createElement('span');
  text.className = 'transport__label';
  text.textContent = 'Rock the card';
  rockLabel.append(rockBox, text);
  transport.append(rockLabel);
}
mounted?.params?.addEventListener('change', (event) => {
  if (event.detail?.id === 'opacity' && !rock.setting && rock.on) {
    rock.on = false;
    if (rockBox) rockBox.checked = false;
  }
});

// The statistics line, under the transport.
const statLine = document.createElement('p');
statLine.className = 'stage__status';
statLine.setAttribute('aria-live', 'off');
statLine.dataset.role = 'lenticular-stats';
document.querySelector('.stage')?.append(statLine);
if (statLine.isConnected) {
  const tick = () => {
    if (statLine.textContent !== stats.text) statLine.textContent = stats.text;
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}

// For a headless check: the page, the rock switch and the render hook.
window.__lenticularDemo = { demo: mounted, rock, hooks };

export { mounted };
