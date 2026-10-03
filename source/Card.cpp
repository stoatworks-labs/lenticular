#include "Card.h"

#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace lenticular::card
{

//---------------------------------------------------------------------------
// The setup and the uniforms. Moved here unchanged from ProcessOpenGL, which
// now calls them: the GPU and the CPU twin are handed the same floats.
//---------------------------------------------------------------------------
lens::Setup SetupFor( const HostValues& v )
{
	lens::Setup s;
	s.squeeze       = v.squeeze > 0.5f;
	s.printPerWidth = PitchFromParam( v.interleavePitch );
	s.lensPerWidth  = PitchFromParam( v.lensPitch );
	//Exactly 1.0 when the two sliders agree: the same float through the same
	//pow is the same double.
	s.ratio         = s.printPerWidth / s.lensPerWidth;
	s.focal         = FocalFromParam( v.focalLength );
	s.spotLens      = SpotFromParam( v.focusSpot );
	s.spotPeriods   = std::min( s.spotLens * s.ratio, lens::kSpotPeriodsMax );
	s.bleedPeriods  = 0.5 * BleedFromParam( v.bleed );//a strip is half a period
	s.invDistance   = InvDistanceFromParam( v.distance );
	s.tilt          = TiltFromParams( v.opacity, v.angleRange );
	s.shine         = std::clamp( static_cast< double >( v.ridgeShine ), 0.0, 1.0 );
	s.light         = LightFromParam( v.lightAngle );
	s.distort       = lens::kDistortMax * s.shine;
	return s;
}

Uniforms UniformsFor( const lens::Setup& s, int outW, int outH )
{
	const double degree         = kPi / 180.0;
	const double highlightWidth = std::max( lens::kHighlightWidth, lens::kHighlightMinPx * s.lensPerWidth / outW );

	Uniforms u;
	u.outW = outW;
	u.outH = outH;

	u.lensPerWidth  = static_cast< float >( s.lensPerWidth );
	u.printPerWidth = static_cast< float >( s.printPerWidth );
	u.pitchRatio    = static_cast< float >( s.ratio );
	u.focal         = static_cast< float >( s.focal );
	u.spotPeriods   = static_cast< float >( s.spotPeriods );
	u.bleedPeriods  = static_cast< float >( s.bleedPeriods );
	u.squeeze       = s.squeeze ? 1 : 0;

	u.sinTilt     = static_cast< float >( std::sin( s.tilt ) );
	u.cosTilt     = static_cast< float >( std::cos( s.tilt ) );
	u.invDistance = static_cast< float >( s.invDistance );

	u.shine          = static_cast< float >( s.shine );
	u.lightAngle     = static_cast< float >( s.light );
	u.distort        = static_cast< float >( s.distort );
	u.ridgeSlope     = static_cast< float >( std::sin( lens::kRidgeEdgeSlopeDeg * degree ) );
	u.highlightWidth = static_cast< float >( highlightWidth );
	u.edgeShade      = static_cast< float >( lens::kEdgeShade );
	return u;
}

Texture MakeTexture( const float* rgba, int width, int height )
{
	Texture t;
	t.rgba   = rgba;
	t.width  = width;
	t.height = height;
	//As ProcessOpenGL computes HalfTexelA and HalfTexelB: in float.
	t.halfTexelX = 0.5f / static_cast< float >( width );
	t.halfTexelY = 0.5f / static_cast< float >( height );
	return t;
}

//---------------------------------------------------------------------------
// The sampler. Not GLSL, but the GL's: the level-0 LINEAR equation of the
// OpenGL 4.1 specification (8.14.2), with CLAMP_TO_EDGE on both axes.
//
//     i0 = floor( u - 1/2 ), alpha = frac( u - 1/2 ), u = s W     (and j, beta, v)
//     tau = (1-a)(1-b) T[i0,j0] + a(1-b) T[i1,j0] + (1-a)b T[i0,j1] + ab T[i1,j1]
//
// A GPU is allowed to quantise alpha and beta (to 8 bits on much hardware);
// this does not. That is the largest single difference between the twins,
// and lntest --cpu bounds it.
//---------------------------------------------------------------------------
namespace
{
inline const float* texel( const Texture& t, int i, int j )
{
	i = std::min( std::max( i, 0 ), t.width - 1 );
	j = std::min( std::max( j, 0 ), t.height - 1 );
	return t.rgba + ( static_cast< size_t >( j ) * static_cast< size_t >( t.width ) + static_cast< size_t >( i ) ) * 4;
}
} // namespace

void Sample( const Texture& t, float s, float tt, float out[ 4 ], int fault )
{
	if( t.rgba == nullptr || t.width <= 0 || t.height <= 0 )
	{
		out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 0.0f;
		return;
	}

	const float u = s * static_cast< float >( t.width );
	const float v = tt * static_cast< float >( t.height );

	if( ( fault & kFaultNearestTexel ) != 0 )
	{
		//The negative control: GL_NEAREST.
		const float* p = texel( t, static_cast< int >( std::floor( u ) ), static_cast< int >( std::floor( v ) ) );
		for( int c = 0; c < 4; ++c )
			out[ c ] = p[ c ];
		return;
	}

	const float x  = u - 0.5f;
	const float y  = v - 0.5f;
	const float fx = std::floor( x );
	const float fy = std::floor( y );
	const float a  = x - fx;
	const float b  = y - fy;
	const int i0   = static_cast< int >( fx );
	const int j0   = static_cast< int >( fy );

	const float* t00 = texel( t, i0, j0 );
	const float* t10 = texel( t, i0 + 1, j0 );
	const float* t01 = texel( t, i0, j0 + 1 );
	const float* t11 = texel( t, i0 + 1, j0 + 1 );

	const float w00 = ( 1.0f - a ) * ( 1.0f - b );
	const float w10 = a * ( 1.0f - b );
	const float w01 = ( 1.0f - a ) * b;
	const float w11 = a * b;
	for( int c = 0; c < 4; ++c )
		out[ c ] = w00 * t00[ c ] + w10 * t10[ c ] + w01 * t01[ c ] + w11 * t11[ c ];
}

//---------------------------------------------------------------------------
// The shader's per-pixel stage, transcribed. Each function carries the name
// of its GLSL twin in kLenticularShader; read the two side by side.
//---------------------------------------------------------------------------
namespace
{
inline float clampf( float x, float lo, float hi )
{
	return std::min( std::max( x, lo ), hi );
}

//= mirrored -- fetchA / fetchB (Fault 0, MaxUV 1: a host buffer has no padding)
inline void fetch( const Texture& t, float px, float py, float out[ 4 ], int fault )
{
	const float qx = clampf( px, t.halfTexelX, 1.0f - t.halfTexelX );
	const float qy = clampf( py, t.halfTexelY, 1.0f - t.halfTexelY );
	Sample( t, qx, qy, out, fault );
}

//= mirrored -- stepUp
inline float stepUp( float x, float bleed )
{
	if( bleed <= 0.0f )
		return x >= 0.0f ? 1.0f : 0.0f;
	return clampf( 0.5f + x / bleed, 0.0f, 1.0f );
}

//= mirrored -- stripB
inline float stripB( float u, float bleed )
{
	return 1.0f - stepUp( u, bleed ) + stepUp( u - 0.5f, bleed ) - stepUp( u - 1.0f, bleed );
}

//= mirrored -- rampIntegral
inline float rampIntegral( float x, float bleed )
{
	const float h = 0.5f * bleed;
	if( x <= -h )
		return 0.0f;
	if( x >= h )
		return x;
	return ( x + h ) * ( x + h ) / ( 2.0f * bleed );
}

//= mirrored -- integralB
inline float integralB( float x, float bleed )
{
	const float n    = std::floor( x );
	const float u    = x - n;
	const float part = u - ( rampIntegral( u, bleed ) - rampIntegral( 0.0f, bleed ) ) + rampIntegral( u - 0.5f, bleed )
	                   - rampIntegral( u - 1.0f, bleed );
	return 0.5f * n + part;
}

//= mirrored -- coverageB
inline float coverageB( float phase, float w, float bleed )
{
	const float lo = phase - 0.5f * w;
	const float n  = std::floor( lo );
	const float a  = lo - n;
	const float b  = a + w;
	const float h  = 0.5f * bleed;
	if( b <= 1.0f )
	{
		if( a >= 0.5f + h && b <= 1.0f - h )
			return 1.0f;
		if( a >= h && b <= 0.5f - h )
			return 0.0f;
	}
	if( w <= 1.0e-5f )
		return stripB( phase - std::floor( phase ), bleed );
	return ( integralB( b, bleed ) - integralB( a, bleed ) ) / w;
}
} // namespace

//= mirrored -- main
void Shade( const Uniforms& U, const Texture& A, const Texture& B, int pxi, int pyi, float out[ 4 ] )
{
	//The pixel, as integers, and its centre as a fraction of the picture.
	const float pxf = static_cast< float >( pxi );
	const float pyf = static_cast< float >( pyi );
	const float X   = ( pxf + 0.5f ) / static_cast< float >( U.outW );
	const float Y   = ( pyf + 0.5f ) / static_cast< float >( U.outH );

	//The lens this column is under, and where across it.
	const float xl = X * U.lensPerWidth;
	const float k  = std::floor( xl );
	const float e  = xl - k - 0.5f;

	//The angle this column is seen at.
	const float across  = ( X - 0.5f ) * U.invDistance;
	const float tanView = ( U.sinTilt - across ) / U.cosTilt;
	const float reach   = tanView;

	//Where the lens focuses on the print, as a whole number of periods plus a
	//lead under two periods.
	const float base = ( k + 0.5f ) * U.pitchRatio;
	const float nb   = std::floor( base );
	const float lead = ( base - nb ) + ( U.focal * reach + U.distort * e ) * U.pitchRatio;
	const float nl   = std::floor( lead );
	const float u    = lead - nl;
	const float n    = nb + nl;

	//What is printed there.
	float posA = n + u;
	float posB = n + u;
	if( U.squeeze != 0 )
	{
		posA = n + std::min( 2.0f * u, 1.0f );
		posB = n + std::max( 2.0f * u - 1.0f, 0.0f );
	}

	const float m = coverageB( lead, U.spotPeriods, U.bleedPeriods );
	float colour[ 4 ];
	if( m <= 0.0f )
		fetch( A, posA / U.printPerWidth, Y, colour, U.fault );
	else if( m >= 1.0f )
		fetch( B, posB / U.printPerWidth, Y, colour, U.fault );
	else
	{
		float ca[ 4 ], cb[ 4 ];
		fetch( A, posA / U.printPerWidth, Y, ca, U.fault );
		fetch( B, posB / U.printPerWidth, Y, cb, U.fault );
		//GLSL's mix, as the specification defines it: x (1 - a) + y a.
		for( int c = 0; c < 4; ++c )
			colour[ c ] = ca[ c ] * ( 1.0f - m ) + cb[ c ] * m;
	}

	//The ridge, composited OVER as a white of coverage h.
	if( U.shine > 0.0f )
	{
		const float view   = std::atan( tanView );
		const float bisect = 0.5f * ( U.lightAngle + view );
		const float spotAt = std::sin( bisect ) / ( 2.0f * U.ridgeSlope );
		const float g      = ( e - spotAt ) / U.highlightWidth;
		const float h      = clampf( U.shine * std::exp( -g * g ), 0.0f, 1.0f );
		const float shade  = 1.0f - U.edgeShade * U.shine * 4.0f * e * e;
		for( int c = 0; c < 3; ++c )
			colour[ c ] *= shade;
		for( int c = 0; c < 4; ++c )
			colour[ c ] = h + ( 1.0f - h ) * colour[ c ];
	}

	for( int c = 0; c < 4; ++c )
		out[ c ] = colour[ c ];
}

void Render( const Uniforms& uniforms, const Texture& a, const Texture& b, float* out, int rowBegin, int rowEnd )
{
	for( int y = rowBegin; y < rowEnd; ++y )
	{
		float* row = out + static_cast< size_t >( y ) * static_cast< size_t >( uniforms.outW ) * 4;
		for( int x = 0; x < uniforms.outW; ++x )
			Shade( uniforms, a, b, x, y, row + static_cast< size_t >( x ) * 4 );
	}
}

} // namespace lenticular::card
