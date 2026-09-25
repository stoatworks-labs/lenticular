#include "Shaders.h"

namespace lenticular
{

//---------------------------------------------------------------------------
// The vertex shader passes UV through UNSCALED. The SDK's mixer example
// folds each input's MaxUV in here; that is only right for a plugin whose
// every fetch is at the fragment's own position, and it is the arrangement
// genlock's --mixer check exists to catch. Every fetch here is at a position
// the lens chose, so picture space is what the fragment shader gets.
//
// The one exception is the harness's negative control, which folds A's
// MaxUV in exactly the way the trap does, so --mixer can be shown to fail.
//---------------------------------------------------------------------------
const char* const kVertexShader = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// The card. See Lens.h for the geometry.
//---------------------------------------------------------------------------
const char* const kLenticularShader = R"(#version 410 core

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
)";

} // namespace lenticular
