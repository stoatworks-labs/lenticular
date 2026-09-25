/**
    lntest -- render Lenticular offline, and measure what the card is doing.

    A harness that drives **two inputs**, because Lenticular is a mixer.
    `inputTextures[0]` is Dest, the layer below -- A, printed in each lens's
    first strip. `inputTextures[1]` is Src, this layer -- B, in the second.
    They are `--input-a` and `--input-b` here, and they may be different
    sizes, with different hardware padding, rendered to an output that is a
    third size.

        lntest --out /tmp/frame.png     a picture, through the real plugin
        lntest --list                   every parameter, for the sweep
        lntest --names                  no name over 16 characters or duplicated; index 0 is Squeeze
        lntest --mixer                  two inputs, two MaxUVs, and the guards
        lntest --ends                   tilted all the way, A or B sampled at the lens pitch, bitwise
        lntest --flip                   the whole card flips at the angles the lens predicts
        lntest --moire                  a pitch mismatch bands at 1/|1/p_L - 1/p_P|, by FFT
        lntest --distance               a near viewer: the flip sweeps across the card as D sin T
        lntest --opacity                Opacity tilts the card monotonically from -max to +max
        lntest --mutation               one character of the shipped GLSL fails a check
        lntest --bench                  ms/frame at 720p through 4K
        lntest --pipe                   raw frames in, raw frames out (two inputs)

    LNTEST_RENDERER=software in the environment renders on Apple's software
    renderer instead of the GPU: what a GPU-less CI runner gets.

    Every check renders through the REAL plugin class in a headless CGL
    context and measures the property out of the picture -- a flip read off
    a ramp, a band period read off a spectrum, a boundary read off a row's
    sum -- against the closed form the lens geometry gives. Every check runs
    at 640x360 and at 320x180, CI's raster, and carries a negative control
    that builds the wrong answer into the real plugin (`SetFaultForTest`)
    and shows the check rejects it.

    ------------------------------------------------------- about the numbers

    Every tolerance is derived from something stated -- one float ULP
    through a stated number of operations, one source texel, one lens pitch,
    the Hann window's leakage from the picture's own other spectral lines, a
    sum-versus-integral residual at a ramp's two kinks -- and never from the
    number this machine printed first. Where a bound is computed, the check
    asserts that the tolerance is at least three times it. AGENTS.md lists
    them all.
*/

#include "Controls.h"
#include "Lens.h"
#include "Lenticular.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace lenticular;

namespace
{
int failures = 0;
bool quiet   = false;

void Check( bool ok, const std::string& what )
{
	if( !quiet )
		std::printf( "  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str() );
	if( !ok )
		++failures;
}

/// A negative control: something the check must be able to REJECT. Passes
/// when `rejected` is true.
void Negative( bool rejected, const std::string& what )
{
	if( !quiet )
		std::printf( "  %s  negative control: %s\n", rejected ? "ok  " : "FAIL", what.c_str() );
	if( !rejected )
		++failures;
}

/// EVERY conversion must be a floating-point one.
__attribute__( ( format( printf, 1, 0 ) ) ) std::string fmt( const char* format, double a, double b = 0.0, double c = 0.0, double d = 0.0 )
{
	char buffer[ 320 ];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
	std::snprintf( buffer, sizeof( buffer ), format, a, b, c, d );
#pragma clang diagnostic pop
	return buffer;
}

/// Run a check silently and return how many of its assertions failed,
/// leaving the global count as it was: how a negative control asks "did
/// the check reject this?".
template< typename F >
int failuresOf( F&& run )
{
	const int before = failures;
	quiet            = true;
	run();
	quiet            = false;
	const int caught = failures - before;
	failures         = before;
	return caught;
}

constexpr double kFloatUlp = 1.0 / 16777216.0;///< 2^-24, float32's unit roundoff at 1
constexpr double kDegree   = kPi / 180.0;

//---------------------------------------------------------------------------
// PNG writer. zlib ships with the OS. Rows BOTTOM-UP in, top-down out.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

using Image  = std::vector< unsigned char >;
using ImageF = std::vector< float >;

bool writePng( const std::string& path, int width, int height, const Image& bottomUp )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		const unsigned char* row = bottomUp.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Pictures. Bottom-up, GL's orientation.
//---------------------------------------------------------------------------
struct Rgb
{
	unsigned char r, g, b;
};

unsigned char toByte( float v )
{
	return static_cast< unsigned char >( std::lround( std::min( 1.0f, std::max( 0.0f, v ) ) * 255.0f ) );
}

constexpr Rgb kBlack = { 0, 0, 0 };
constexpr Rgb kWhite = { 255, 255, 255 };

void put( Image& image, int width, int x, int y, Rgb c, unsigned char a = 255 )
{
	const size_t at = ( static_cast< size_t >( y ) * width + x ) * 4;
	image[ at + 0 ] = c.r;
	image[ at + 1 ] = c.g;
	image[ at + 2 ] = c.b;
	image[ at + 3 ] = a;
}

Rgb pixelAt( const Image& image, int width, int x, int y )
{
	const size_t at = ( static_cast< size_t >( y ) * width + x ) * 4;
	return { image[ at + 0 ], image[ at + 1 ], image[ at + 2 ] };
}

int l1( Rgb a, Rgb b )
{
	return std::abs( a.r - b.r ) + std::abs( a.g - b.g ) + std::abs( a.b - b.b );
}

Image flatField( int width, int height, Rgb c, unsigned char alpha = 255 )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
			put( image, width, x, y, c, alpha );
	return image;
}

void hsv( float h, float s, float v, float& r, float& g, float& b )
{
	const float c = v * s;
	const float x = c * ( 1.0f - std::fabs( std::fmod( h * 6.0f, 2.0f ) - 1.0f ) );
	const float m = v - c;
	float rr = 0, gg = 0, bb = 0;
	const int sector = static_cast< int >( h * 6.0f ) % 6;
	switch( sector )
	{
	case 0: rr = c; gg = x; break;
	case 1: rr = x; gg = c; break;
	case 2: gg = c; bb = x; break;
	case 3: gg = x; bb = c; break;
	case 4: rr = x; bb = c; break;
	default: rr = c; bb = x; break;
	}
	r = rr + m;
	g = gg + m;
	b = bb + m;
}

/// A: bars across the top, a grey ramp, a hue field -- and an alpha that is
/// NOT 255 everywhere (a translucent strip down the right), because
/// Resolume's demo clips are DXV with alpha and a plugin that mishandles
/// alpha looks fine on an opaque card.
Image videoCard( int width, int height )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const float u = ( x + 0.5f ) / width;
			const float v = ( y + 0.5f ) / height;
			float r = 0, g = 0, b = 0;
			if( v > 0.72f )
			{
				static const float bars[ 7 ][ 3 ] = {
					{ 0.75f, 0.75f, 0.75f }, { 0.75f, 0.75f, 0.0f }, { 0.0f, 0.75f, 0.75f }, { 0.0f, 0.75f, 0.0f },
					{ 0.75f, 0.0f, 0.75f }, { 0.75f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.75f },
				};
				const int which = std::min( 6, static_cast< int >( u * 7.0f ) );
				r = bars[ which ][ 0 ];
				g = bars[ which ][ 1 ];
				b = bars[ which ][ 2 ];
			}
			else if( v > 0.58f )
				r = g = b = u;
			else
				hsv( u, 0.8f, 0.3f + 0.65f * ( v / 0.58f ), r, g, b );
			put( image, width, x, y, { toByte( r ), toByte( g ), toByte( b ) }, u > 0.9f ? 96 : 255 );
		}
	return image;
}

/// B: a warm picture with diagonal stripes, a disc and a dark panel, so a
/// flip has something recognisably different to land on. Its alpha varies
/// too: the disc is half transparent.
Image graphicCard( int width, int height )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	const double cx = 0.62 * width, cy = 0.45 * height, rad = 0.22 * height;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 ) / width, v = ( y + 0.5 ) / height;
			Rgb c;
			unsigned char alpha = 255;
			const int stripe    = static_cast< int >( std::floor( ( u * 1.6 + v ) * 8.0 ) ) & 1;
			c                   = stripe ? Rgb { 0xF2, 0x8C, 0x28 } : Rgb { 0xF7, 0xC5, 0x6B };
			if( u < 0.3 && v > 0.15 && v < 0.85 )
				c = { 0x22, 0x1E, 0x38 };
			const double dx = ( x + 0.5 ) - cx, dy = ( y + 0.5 ) - cy;
			if( dx * dx + dy * dy < rad * rad )
			{
				c     = { 0x2E, 0x8B, 0xC0 };
				alpha = 128;
			}
			put( image, width, x, y, c, alpha );
		}
	return image;
}

/// Twelve vertical bars, each a different red level (16 + 20 j), for the
/// pipe's cue check: which bar a lens samples says what the card did.
Image barsCard( int width, int height )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const int j = std::min( 11, ( x * 12 ) / width );
			put( image, width, x, y, { static_cast< unsigned char >( 16 + 20 * j ), 90, static_cast< unsigned char >( 200 - 12 * j ) } );
		}
	return image;
}

/// Four quadrants of flat colour and one marker square, for `--mixer`.
struct QuadCard
{
	Image image;
	Rgb quadrant[ 4 ];
	Rgb marker;
	double markerCentreU = 0.0;
	double markerCentreV = 0.0;
};

QuadCard quadCard( int width, int height, bool srcSide )
{
	QuadCard card;
	if( srcSide )
	{
		card.quadrant[ 0 ] = { 0x00, 0xB4, 0xB4 };
		card.quadrant[ 1 ] = { 0xB4, 0xB4, 0x00 };
		card.quadrant[ 2 ] = { 0x3C, 0x78, 0xFF };
		card.quadrant[ 3 ] = { 0x60, 0x60, 0x60 };
		card.marker        = { 0xA0, 0xFF, 0x00 };
	}
	else
	{
		card.quadrant[ 0 ] = { 0xC8, 0x00, 0x00 };
		card.quadrant[ 1 ] = { 0x00, 0xC8, 0x00 };
		card.quadrant[ 2 ] = { 0x00, 0x00, 0xC8 };
		card.quadrant[ 3 ] = { 0xF0, 0xE0, 0xC0 };
		card.marker        = { 0xFF, 0x80, 0x00 };
	}
	card.image = Image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const int q = ( y >= height / 2 ? 2 : 0 ) + ( x >= width / 2 ? 1 : 0 );
			put( card.image, width, x, y, card.quadrant[ q ] );
		}
	const int x0 = static_cast< int >( std::lround( 0.1875 * width ) );
	const int x1 = static_cast< int >( std::lround( 0.3125 * width ) );
	const int y0 = static_cast< int >( std::lround( 0.1875 * height ) );
	const int y1 = static_cast< int >( std::lround( 0.3125 * height ) );
	for( int y = y0; y < y1; ++y )
		for( int x = x0; x < x1; ++x )
			put( card.image, width, x, y, card.marker );
	card.markerCentreU = ( 0.5 * ( x0 + x1 ) ) / static_cast< double >( width );
	card.markerCentreV = ( 0.5 * ( y0 + y1 ) ) / static_cast< double >( height );
	return card;
}

/// A PCG-style integer hash: the same bits on every machine.
uint32_t hash32( uint32_t v )
{
	v       = v * 747796405u + 2891336453u;
	uint32_t w = ( ( v >> ( ( v >> 28u ) + 4u ) ) ^ v ) * 277803737u;
	return ( w >> 22u ) ^ w;
}

/// For `--ends`: every lens cell of the card split into three sub-blocks
/// and four row bands, each its own colour AND its own alpha, so the check
/// can say which sub-block of which lens every output pixel came from, in
/// all four channels. `lensPerWidth` is the plugin's own N_L.
struct BlockCard
{
	Image image;
	int width = 0, height = 0;
	double lensPerWidth = 0.0;
	static constexpr int kSub   = 3;
	static constexpr int kBands = 4;

	int SubOf( double x ) const///< x in texels, a position along the row
	{
		const double cell = x * lensPerWidth / width;
		return static_cast< int >( std::floor( ( cell - std::floor( cell ) ) * kSub ) );
	}
	int CellOf( double x ) const
	{
		return static_cast< int >( std::floor( x * lensPerWidth / width ) );
	}
	unsigned char Channel( int cell, int sub, int band, int c ) const
	{
		const uint32_t h = hash32( static_cast< uint32_t >( ( cell * kSub + sub ) * kBands + band ) * 4u + static_cast< uint32_t >( c ) + 17u );
		if( c == 3 )
			return static_cast< unsigned char >( 64 + ( h % 192 ) );//alpha 64..255
		return static_cast< unsigned char >( h & 255u );
	}
};

BlockCard blockCard( int width, int height, double lensPerWidth )
{
	BlockCard card;
	card.width        = width;
	card.height       = height;
	card.lensPerWidth = lensPerWidth;
	card.image        = Image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
	{
		const int band = ( y * BlockCard::kBands ) / height;
		for( int x = 0; x < width; ++x )
		{
			const double centre = x + 0.5;
			const int cell      = card.CellOf( centre );
			const int sub       = card.SubOf( centre );
			for( int c = 0; c < 4; ++c )
				card.image[ ( static_cast< size_t >( y ) * width + x ) * 4 + static_cast< size_t >( c ) ] = card.Channel( cell, sub, band, c );
		}
	}
	return card;
}

Image generate( const std::string& name, int width, int height )
{
	if( name == "video" )
		return videoCard( width, height );
	if( name == "graphic" )
		return graphicCard( width, height );
	if( name == "quads-a" )
		return quadCard( width, height, false ).image;
	if( name == "quads-b" )
		return quadCard( width, height, true ).image;
	if( name == "bars" )
		return barsCard( width, height );
	if( name == "black" )
		return flatField( width, height, kBlack );
	if( name == "white" )
		return flatField( width, height, kWhite );
	return flatField( width, height, { 0x00, 0x99, 0x00 } );//"flat"
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	//LNTEST_RENDERER=software asks for Apple's software renderer by id, on a
	//Mac that has a GPU. It is what a GPU-less CI runner falls back to, so a
	//check that fails only in CI can be reproduced here.
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "LNTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		std::fprintf( stderr, "lntest: LNTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels, bool floatFormat = false )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	if( floatFormat )
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr );
	else
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// The rig: the plugin, TWO input textures and an output framebuffer, each
// at its own size. `hardware` may exceed `used` -- that is what MaxUV is
// for -- and the padding is a sentinel colour that appears nowhere else.
//---------------------------------------------------------------------------
constexpr Rgb kSentinel     = { 0xFF, 0x00, 0xFF };
constexpr int kSentinelNear = 135;

struct InputSpec
{
	int usedW = 0, usedH = 0;
	int hwW = 0, hwH = 0;
	static InputSpec Exact( int w, int h )
	{
		return { w, h, w, h };
	}
	static InputSpec Padded( int w, int h, int hw, int hh )
	{
		return { w, h, hw, hh };
	}
};

struct Rig
{
	Lenticular plugin;
	int width = 0, height = 0;
	InputSpec aSpec, bSpec;

	GLuint aTexture = 0, bTexture = 0;
	GLuint outputTexture = 0, outputFBO = 0;
	FFGLTextureStruct aStruct = {}, bStruct = {};
	FFGLTextureStruct* inputs[ 2 ] = { nullptr, nullptr };
	ProcessOpenGLStruct process    = {};
	bool ready                     = false;

	bool Init( int outW, int outH, InputSpec a, InputSpec b, bool floatOutput = false, int fault = 0, const char* fragment = nullptr )
	{
		width  = outW;
		height = outH;
		aSpec  = a;
		bSpec  = b;

		plugin.SetFaultForTest( fault );
		plugin.SetFragmentShaderForTest( fragment );

		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( outW );
		viewport.height             = static_cast< FFUInt32 >( outH );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for the shader\n" );
			return false;
		}

		aTexture = makeTexture( a.hwW, a.hwH, flatField( a.hwW, a.hwH, kSentinel ).data() );
		bTexture = makeTexture( b.hwW, b.hwH, flatField( b.hwW, b.hwH, kSentinel ).data() );

		outputTexture = makeTexture( outW, outH, nullptr, floatOutput );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );

		aStruct.Width          = static_cast< FFUInt32 >( a.usedW );
		aStruct.Height         = static_cast< FFUInt32 >( a.usedH );
		aStruct.HardwareWidth  = static_cast< FFUInt32 >( a.hwW );
		aStruct.HardwareHeight = static_cast< FFUInt32 >( a.hwH );
		aStruct.Handle         = aTexture;
		bStruct.Width          = static_cast< FFUInt32 >( b.usedW );
		bStruct.Height         = static_cast< FFUInt32 >( b.usedH );
		bStruct.HardwareWidth  = static_cast< FFUInt32 >( b.hwW );
		bStruct.HardwareHeight = static_cast< FFUInt32 >( b.hwH );
		bStruct.Handle         = bTexture;

		inputs[ 0 ]              = &aStruct;
		inputs[ 1 ]              = &bStruct;
		process.numInputTextures = 2;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
		ready                    = true;
		return true;
	}

	bool Init( int outW, int outH, bool floatOutput = false, int fault = 0, const char* fragment = nullptr )
	{
		return Init( outW, outH, InputSpec::Exact( outW, outH ), InputSpec::Exact( outW, outH ), floatOutput, fault, fragment );
	}

	void UploadA( const Image& bottomUp )
	{
		glBindTexture( GL_TEXTURE_2D, aTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, aSpec.usedW, aSpec.usedH, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void UploadB( const Image& bottomUp )
	{
		glBindTexture( GL_TEXTURE_2D, bTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, bSpec.usedW, bSpec.usedH, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	bool Set( const std::string& name, float value )
	{
		for( unsigned int i = 0; i < Lenticular::PT_COUNT; ++i )
		{
			const char* declared = plugin.GetParamName( i );
			if( declared != nullptr && name == declared )
			{
				plugin.SetFloatParameter( i, value );
				return true;
			}
		}
		std::fprintf( stderr, "no parameter called '%s'\n", name.c_str() );
		return false;
	}

	/// The plugin has no clock, so a frame is a frame. SetTime is still not
	/// needed: nothing in Lenticular moves unless a parameter does.
	bool Render()
	{
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
	}

	FFResult RenderBroken( int numInputs, int nullIndex )
	{
		FFGLTextureStruct* saved[ 2 ] = { inputs[ 0 ], inputs[ 1 ] };
		if( nullIndex >= 0 && nullIndex < 2 )
			inputs[ nullIndex ] = nullptr;
		if( nullIndex == -2 )
			process.inputTextures = nullptr;
		process.numInputTextures = static_cast< FFUInt32 >( numInputs );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		const FFResult result = plugin.ProcessOpenGL( &process );
		inputs[ 0 ]              = saved[ 0 ];
		inputs[ 1 ]              = saved[ 1 ];
		process.inputTextures    = inputs;
		process.numInputTextures = 2;
		return result;
	}

	Image Pixels()
	{
		Image pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return pixels;
	}

	ImageF PixelsF()
	{
		ImageF pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	~Rig()
	{
		if( !ready )
			return;
		plugin.DeInitGL();
		glDeleteFramebuffers( 1, &outputFBO );
		glDeleteTextures( 1, &outputTexture );
		glDeleteTextures( 1, &bTexture );
		glDeleteTextures( 1, &aTexture );
	}
};

//---------------------------------------------------------------------------
// Measurement.
//---------------------------------------------------------------------------
int differingBytes( const Image& a, const Image& b )
{
	int n = 0;
	for( size_t i = 0; i < a.size() && i < b.size(); ++i )
		if( a[ i ] != b[ i ] )
			++n;
	return n;
}

void meanOver( const Image& image, int width, int x0, int y0, int x1, int y1, double out[ 3 ] )
{
	double sum[ 3 ] = { 0, 0, 0 };
	long n          = 0;
	for( int y = y0; y < y1; ++y )
		for( int x = x0; x < x1; ++x )
		{
			const size_t at = ( static_cast< size_t >( y ) * width + x ) * 4;
			sum[ 0 ] += image[ at + 0 ];
			sum[ 1 ] += image[ at + 1 ];
			sum[ 2 ] += image[ at + 2 ];
			++n;
		}
	for( int c = 0; c < 3; ++c )
		out[ c ] = n > 0 ? sum[ c ] / n : -1.0;
}

/// The red channel of every pixel of a float render, and its spread: a
/// card of flat black A and flat white B renders m, the B fraction, in all
/// three colour channels.
struct Field
{
	std::vector< double > m;///< per pixel, bottom-up
	double lo = 0.0, hi = 0.0, mean = 0.0;
	bool uniformBits = false;///< every pixel the same float, bit for bit
};

Field fieldOf( const ImageF& px, int W, int H )
{
	Field f;
	f.m.resize( static_cast< size_t >( W ) * H );
	f.lo = 1e9;
	f.hi = -1e9;
	double sum = 0.0;
	uint32_t first = 0;
	std::memcpy( &first, &px[ 0 ], 4 );
	f.uniformBits = true;
	for( size_t i = 0; i < f.m.size(); ++i )
	{
		const float r = px[ i * 4 ];
		uint32_t bits = 0;
		std::memcpy( &bits, &r, 4 );
		if( bits != first )
			f.uniformBits = false;
		f.m[ i ] = r;
		f.lo     = std::min( f.lo, static_cast< double >( r ) );
		f.hi     = std::max( f.hi, static_cast< double >( r ) );
		sum += r;
	}
	f.mean = sum / static_cast< double >( f.m.size() );
	return f;
}

/// The settings every physics check starts from: matched pitches, an ideal
/// lens (no spot, no bleed, no shine), the viewer at infinity, flat black A
/// and flat white B -- so a float render IS the B fraction m.
void physicsRig( Rig& rig, double pitch = 60.0 )
{
	rig.UploadA( flatField( rig.aSpec.usedW, rig.aSpec.usedH, kBlack ) );
	rig.UploadB( flatField( rig.bSpec.usedW, rig.bSpec.usedH, kWhite ) );
	rig.Set( "Squeeze", 1.0f );
	rig.Set( "Interleave Pitch", ParamForPitch( pitch ) );
	rig.Set( "Lens Pitch", ParamForPitch( pitch ) );
	rig.Set( "Bleed", 0.0f );
	rig.Set( "Focal Length", ParamForFocal( kFocalDefault ) );
	rig.Set( "Focus Spot", 0.0f );
	rig.Set( "Distance", 1.0f );
	rig.Set( "Ridge Shine", 0.0f );
	rig.Set( "Opacity", 0.5f );
}

/// What the parameters the harness SET stand for, through Controls.h's
/// stated mappings -- the prediction's inputs. Deliberately not the
/// plugin's SetupForTest(), which reports what ProcessOpenGL used and so
/// would follow a fault in it instead of catching it.
lens::Setup asked( Lenticular& plugin )
{
	auto p = [ & ]( unsigned id ) { return plugin.GetFloatParameter( id ); };
	lens::Setup s;
	s.squeeze       = p( Lenticular::PT_SQUEEZE ) > 0.5f;
	s.lensPerWidth  = PitchFromParam( p( Lenticular::PT_LENS_PITCH ) );
	s.printPerWidth = PitchFromParam( p( Lenticular::PT_INTERLEAVE_PITCH ) );
	s.ratio         = s.printPerWidth / s.lensPerWidth;
	s.focal         = FocalFromParam( p( Lenticular::PT_FOCAL_LENGTH ) );
	s.spotLens      = SpotFromParam( p( Lenticular::PT_FOCUS_SPOT ) );
	s.bleedPeriods  = 0.5 * BleedFromParam( p( Lenticular::PT_BLEED ) );
	s.invDistance   = InvDistanceFromParam( p( Lenticular::PT_DISTANCE ) );
	s.tilt          = TiltFromParams( p( Lenticular::PT_OPACITY ), p( Lenticular::PT_ANGLE_RANGE ) );
	return s;
}

const int kRasters[ 2 ][ 2 ] = { { 640, 360 }, { 320, 180 } };

//---------------------------------------------------------------------------
// --list, --names
//---------------------------------------------------------------------------
int runList()
{
	Lenticular plugin;
	std::printf( "%-4s %-22s %-9s %10s   %-16s %s\n", "id", "name", "kind", "value", "range", "means" );
	for( unsigned int id = 0; id < Lenticular::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( id >= Lenticular::PT_ABOUT_FIRST )
		{
			std::printf( "%-4u %-22s %-9s %10s   %-16s %s\n", id, name ? name : "", "about", "-", "-",
			             "the Stoatworks About block; not swept" );
			continue;
		}
		const char* kind = "standard";
		switch( plugin.GetParamType( id ) )
		{
		case FF_TYPE_BOOLEAN: kind = "boolean"; break;
		case FF_TYPE_EVENT: kind = "event"; break;
		case FF_TYPE_INTEGER: kind = "integer"; break;
		case FF_TYPE_OPTION: kind = "option"; break;
		case FF_TYPE_BUFFER: kind = "buffer"; break;
		case FF_TYPE_TEXT: kind = "text"; break;
		default: break;
		}
		RangeStruct range = plugin.GetParamRange( id );
		//An option's range reads back 0..1 whatever its element count.
		if( plugin.GetParamType( id ) == FF_TYPE_OPTION )
		{
			range.min = 0.0f;
			range.max = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( id ) ) ) - 1.0f;
		}
		char rangeText[ 32 ] = {};
		std::snprintf( rangeText, sizeof( rangeText ), "[%g .. %g]", range.min, range.max );
		std::printf( "%-4u %-22s %-9s %10.4f   %-16s\n", id, name ? name : "", kind, plugin.GetFloatParameter( id ), rangeText );
	}
	return 0;
}

int runNames()
{
	Lenticular plugin;
	std::printf( "names longer than FFGL's 16 characters, and duplicates:\n\n" );
	int over = 0;
	std::vector< std::string > seen;
	for( unsigned int id = 0; id < Lenticular::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name != nullptr && std::strlen( name ) > 16 )
		{
			std::printf( "  %-3u  %-28s %zu\n", id, name, std::strlen( name ) );
			++over;
		}
		if( name != nullptr )
		{
			if( std::find( seen.begin(), seen.end(), std::string( name ) ) != seen.end() )
			{
				std::printf( "  %-3u  %-28s duplicate\n", id, name );
				++over;
			}
			seen.push_back( name );
		}
	}
	std::printf( "\n  %d over the limit or duplicated, of %u names\n", over, static_cast< unsigned >( Lenticular::PT_COUNT ) );

	//Index 0 is sacrificial: Resolume Arena does not expose a mixer's first
	//parameter (measured on genlock, wipe and relay), so it must be a
	//control whose default is right for ever. This checks the declaration;
	//only Arena can say which parameter it actually hides.
	const char* first  = plugin.GetParamName( 0 );
	const bool firstOk = first != nullptr && std::strcmp( first, "Squeeze" ) == 0 && plugin.GetParamType( 0 ) == FF_TYPE_BOOLEAN
	                     && plugin.GetFloatParameter( 0 ) == 1.0f;
	std::printf( "  %s  parameter 0, which Arena hides, is %s (boolean, default on)\n", firstOk ? "ok  " : "FAIL", first ? first : "(none)" );
	const char* opacity  = plugin.GetParamName( Lenticular::PT_OPACITY );
	const bool opacityOk = opacity != nullptr && std::strcmp( opacity, "Opacity" ) == 0 && plugin.GetParamType( Lenticular::PT_OPACITY ) == FF_TYPE_STANDARD;
	std::printf( "  %s  the tilt is named Opacity, which Arena binds to the layer fader\n", opacityOk ? "ok  " : "FAIL" );
	const bool matched = plugin.GetFloatParameter( Lenticular::PT_LENS_PITCH ) == plugin.GetFloatParameter( Lenticular::PT_INTERLEAVE_PITCH );
	std::printf( "  %s  the two pitches default to the same slider position (matched, no moire)\n", matched ? "ok  " : "FAIL" );
	return over == 0 && firstOk && opacityOk && matched ? 0 : 1;
}

//---------------------------------------------------------------------------
// --mixer: genlock's two checks, re-run here through a lens, with the fold
// of A's MaxUV into the vertex shader as the negative control.
//---------------------------------------------------------------------------
int mixerCheck( int outW, int outH, int fault )
{
	const InputSpec a = InputSpec::Padded( 200, 120, 256, 256 );
	const InputSpec b = InputSpec::Padded( 96, 70, 128, 128 );
	const QuadCard aCard = quadCard( a.usedW, a.usedH, false );
	const QuadCard bCard = quadCard( b.usedW, b.usedH, true );

	//A fine sheet, so the picture is sampled every two output pixels, at
	//each lens's centre: Squeeze on, the focus a quarter of a lens off-axis
	//at the ends, which is the middle of the strip and maps to the middle of
	//the lens's band of the picture.
	const double lenses = outW / 2.0;
	Rig rig;
	if( !rig.Init( outW, outH, a, b, false, fault ) )
		return 1;
	rig.UploadA( aCard.image );
	rig.UploadB( bCard.image );
	physicsRig( rig, lenses );
	rig.UploadA( aCard.image );
	rig.UploadB( bCard.image );
	const double F = FocalFromParam( ParamForFocal( kFocalDefault ) );
	rig.Set( "Angle Range", ParamForAngleRange( std::atan( 0.25 / F ) ) );
	const double lensPx = outW / PitchFromParam( ParamForPitch( lenses ) );
	if( !quiet )
	{
		std::printf( "  out %dx%d, %g lenses (%.2f px each)\n", outW, outH, PitchFromParam( ParamForPitch( lenses ) ), lensPx );
		std::printf( "  A %dx%d used of %dx%d  (MaxUV %.5f, %.5f)\n", a.usedW, a.usedH, a.hwW, a.hwH,
		             static_cast< double >( a.usedW ) / a.hwW, static_cast< double >( a.usedH ) / a.hwH );
		std::printf( "  B %dx%d used of %dx%d  (MaxUV %.5f, %.5f)\n", b.usedW, b.usedH, b.hwW, b.hwH,
		             static_cast< double >( b.usedW ) / b.hwW, static_cast< double >( b.usedH ) / b.hwH );
	}

	struct Side
	{
		const char* name;
		float opacity;
		const QuadCard* card;
		int usedW, usedH;
	};
	const Side sides[ 2 ] = { { "A", 0.0f, &aCard, a.usedW, a.usedH }, { "B", 1.0f, &bCard, b.usedW, b.usedH } };

	//The third render is a frame that fetches BOTH inputs, which is where a
	//shared or folded MaxUV shows in the picture: a viewer one picture width
	//away at no tilt sees the right half of the card on A and the left on B.
	for( int pass = 0; pass < 3; ++pass )
	{
		const bool mid   = pass == 2;
		const Side& side = sides[ mid ? 0 : pass ];
		rig.Set( "Opacity", mid ? 0.5f : side.opacity );
		rig.Set( "Distance", mid ? ParamForDistance( 1.0 ) : 1.0f );
		if( !rig.Render() )
		{
			Check( false, std::string( side.name ) + ": ProcessOpenGL failed" );
			continue;
		}
		const Image out         = rig.Pixels();
		const std::string label = mid ? "  A|B at a near viewer" : std::string( "  " ) + side.name;

		int sentinelPixels = 0;
		for( int y = 0; y < outH; ++y )
			for( int x = 0; x < outW; ++x )
				if( l1( pixelAt( out, outW, x, y ), kSentinel ) < kSentinelNear )
					++sentinelPixels;
		if( !mid )
		{
			int nearestCard = 1000;
			for( int q = 0; q < 4; ++q )
				nearestCard = std::min( nearestCard, l1( side.card->quadrant[ q ], kSentinel ) );
			nearestCard = std::min( nearestCard, l1( side.card->marker, kSentinel ) );
			Check( nearestCard >= 2 * kSentinelNear,
			       label + fmt( ": no card colour is within %.0f of the padding (needs %.0f)", nearestCard, 2.0 * kSentinelNear ) );
		}
		Check( sentinelPixels == 0, label + fmt( ": no texture padding reached the picture (%.0f sentinel pixels)", sentinelPixels ) );
		if( mid )
		{
			int fromA = 0, fromB = 0;
			for( int y = 0; y < outH; ++y )
				for( int x = 0; x < outW; ++x )
				{
					const Rgb p = pixelAt( out, outW, x, y );
					for( int q = 0; q < 4; ++q )
					{
						if( l1( p, aCard.quadrant[ q ] ) < 20 )
							++fromA;
						if( l1( p, bCard.quadrant[ q ] ) < 20 )
							++fromB;
					}
				}
			Check( fromA > 0 && fromB > 0, label + fmt( ": both inputs are in the frame (%.0f A-ish, %.0f B-ish pixels)", fromA, fromB ) );
			continue;
		}

		//A boundary can smear over one source texel of output and move by
		//up to one lens (the picture is sampled at each lens's centre).
		const int insetX = static_cast< int >( std::ceil( static_cast< double >( outW ) / side.usedW ) + std::ceil( lensPx ) ) + 1;
		const int insetY = static_cast< int >( std::ceil( static_cast< double >( outH ) / side.usedH ) ) + 1;
		double worst     = 0.0;
		for( int q = 1; q < 4; ++q )
		{
			const int qx = ( q & 1 ) ? outW / 2 : 0;
			const int qy = ( q & 2 ) ? outH / 2 : 0;
			double mean[ 3 ];
			meanOver( out, outW, qx + insetX, qy + insetY, qx + outW / 2 - insetX, qy + outH / 2 - insetY, mean );
			const Rgb want = side.card->quadrant[ q ];
			worst          = std::max( { worst, std::fabs( mean[ 0 ] - want.r ), std::fabs( mean[ 1 ] - want.g ), std::fabs( mean[ 2 ] - want.b ) } );
		}
		Check( worst <= 1.0, label + fmt( ": each quadrant is its own flat colour (worst %.3f of 255, tolerance 1)", worst ) );

		double sx = 0.0, sy = 0.0;
		long n    = 0;
		const int halfDistance = l1( side.card->marker, side.card->quadrant[ 0 ] ) / 2;
		for( int y = 0; y < outH / 2; ++y )
			for( int x = 0; x < outW / 2; ++x )
				if( l1( pixelAt( out, outW, x, y ), side.card->marker ) < halfDistance )
				{
					sx += x + 0.5;
					sy += y + 0.5;
					++n;
				}
		const double gotU = n > 0 ? sx / n / outW : -1.0;
		const double gotV = n > 0 ? sy / n / outH : -1.0;
		//One source texel, and in x half a lens more: a marker edge lands on
		//whichever lens's centre it covers.
		const double tolU = 1.0 / side.usedW + 0.5 * lensPx / outW, tolV = 1.0 / side.usedH;
		Check( n > 0 && std::fabs( gotU - side.card->markerCentreU ) <= tolU && std::fabs( gotV - side.card->markerCentreV ) <= tolV,
		       label + fmt( ": the marker is where it was put (%.4f, %.4f", gotU, gotV )
		           + fmt( " vs %.4f, %.4f; tolerance a source texel (+ half a lens in x) %.4f, %.4f)", side.card->markerCentreU,
		                  side.card->markerCentreV, tolU, tolV ) );
	}
	return 0;
}

int runMixer()
{
	std::printf( "a mixer takes two inputs, of two sizes, with two MaxUVs\n\n" );
	{
		Rig rig;
		if( !rig.Init( 320, 200 ) )
			return 1;
		rig.UploadA( videoCard( 320, 200 ) );
		rig.UploadB( graphicCard( 320, 200 ) );
		Check( rig.RenderBroken( 2, -2 ) == FF_FAIL, "a null input ARRAY returns FF_FAIL" );
		Check( rig.RenderBroken( 0, -1 ) == FF_FAIL, "zero input textures returns FF_FAIL" );
		Check( rig.RenderBroken( 1, -1 ) == FF_FAIL, "one input texture returns FF_FAIL" );
		Check( rig.RenderBroken( 2, 0 ) == FF_FAIL, "a null A returns FF_FAIL" );
		Check( rig.RenderBroken( 2, 1 ) == FF_FAIL, "a null B returns FF_FAIL" );
		Check( rig.Render(), "and two real inputs still render afterwards" );
	}
	//genlock's raster, and CI's.
	const int outs[ 2 ][ 2 ] = { { 320, 200 }, { 320, 180 } };
	for( const auto& o : outs )
	{
		std::printf( "\n" );
		if( mixerCheck( o[ 0 ], o[ 1 ], kFaultNone ) != 0 )
			return 1;
		//The negative control: A's MaxUV folded into the vertex shader -- the
		//genlock trap -- must fail this check.
		const int caught = failuresOf( [ & ] { mixerCheck( o[ 0 ], o[ 1 ], kFaultFoldedMaxUV ); } );
		Negative( caught > 0, fmt( "A's MaxUV folded into the vertex shader fails %.0f assertion(s)", caught ) );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --ends: tilted all the way, the card is A (or B) sampled at the lens
// pitch, bitwise, in all four channels, exactly where the interleave says.
//---------------------------------------------------------------------------

/// Where the plugin's own setup says lens k's focus lands at this tilt,
/// the harness's prediction of which picture and which texel column it
/// shows, and how far that is from a sub-block edge. Written from the
/// geometry in Lens.h, in double: the print phase, its strip, what the
/// strip holds.
struct EndPrediction
{
	bool onB     = false;
	double texel = 0.0;///< the sample position along the row, in texels
	double margin = 0.0;///< from the nearest sub-block edge, in texels
};

EndPrediction predictEnd( const lens::Setup& s, int k, int W, const BlockCard& card )
{
	EndPrediction p;
	const double phase = ( k + 0.5 + s.focal * std::tan( s.tilt ) ) * s.ratio;
	const double n     = std::floor( phase );
	const double u     = phase - n;
	p.onB              = u >= 0.5;
	double pos         = phase;
	if( s.squeeze )
		pos = p.onB ? n + ( 2.0 * u - 1.0 ) : n + 2.0 * u;
	p.texel = pos / s.printPerWidth * W;
	//Distance to the nearest sub-block edge, in texels.
	const double cell = p.texel * card.lensPerWidth / W;
	const double f    = ( cell - std::floor( cell ) ) * BlockCard::kSub;
	const double edge = std::min( f - std::floor( f ), std::ceil( f ) - f );
	p.margin          = edge * ( W / card.lensPerWidth ) / BlockCard::kSub;
	return p;
}

int endsCheck( int W, int H, bool squeeze, float opacity, float shine, int& wrongBytes, double& worstMargin )
{
	Rig rig;
	if( !rig.Init( W, H ) )
		return 1;
	const double lenses = W / 20.0;//a lens of 20 whole pixels
	physicsRig( rig, lenses );
	const double F = FocalFromParam( ParamForFocal( kFocalDefault ) );
	rig.Set( "Angle Range", ParamForAngleRange( std::atan( 0.25 / F ) ) );
	rig.Set( "Squeeze", squeeze ? 1.0f : 0.0f );
	rig.Set( "Focus Spot", ParamForSpot( 0.1 ) );//the default: must not reach the other strip
	rig.Set( "Ridge Shine", shine );
	rig.Set( "Opacity", opacity );
	const double N  = PitchFromParam( ParamForPitch( lenses ) );
	const BlockCard aCard = blockCard( W, H, N );
	BlockCard bCard       = blockCard( W, H, N );
	//B's colours differ from A's: offset the hash by re-deriving with a
	//different seed through the band index.
	for( size_t i = 0; i < bCard.image.size(); i += 4 )
		for( int c = 0; c < 3; ++c )
			bCard.image[ i + static_cast< size_t >( c ) ] = static_cast< unsigned char >( 255 - bCard.image[ i + static_cast< size_t >( c ) ] );
	rig.UploadA( aCard.image );
	rig.UploadB( bCard.image );
	if( !rig.Render() )
		return 1;
	const Image out     = rig.Pixels();
	const lens::Setup s = asked( rig.plugin );

	wrongBytes  = 0;
	worstMargin = 1e9;
	for( int x = 0; x < W; ++x )
	{
		const int k             = static_cast< int >( std::floor( ( x + 0.5 ) / W * s.lensPerWidth ) );
		const EndPrediction p   = predictEnd( s, k, W, aCard );
		const BlockCard& card   = p.onB ? bCard : aCard;
		const int col           = std::clamp( static_cast< int >( std::floor( p.texel ) ), 0, W - 1 );
		worstMargin             = std::min( worstMargin, p.margin );
		for( int y = 0; y < H; ++y )
			for( int c = 0; c < 4; ++c )
			{
				const size_t at = ( static_cast< size_t >( y ) * W + static_cast< size_t >( col ) ) * 4 + static_cast< size_t >( c );
				const size_t to = ( static_cast< size_t >( y ) * W + static_cast< size_t >( x ) ) * 4 + static_cast< size_t >( c );
				if( out[ to ] != card.image[ at ] )
					++wrongBytes;
			}
	}
	return 0;
}

int runEnds()
{
	std::printf( "tilted all the way, the card is A or B sampled at the lens pitch, bitwise\n\n" );
	for( const auto& r : kRasters )
	{
		const int W = r[ 0 ], H = r[ 1 ];
		std::printf( "  %dx%d, %d lenses of 20 px, three sub-blocks and four bands a lens, alpha 64..255\n", W, H, W / 20 );
		for( int sq = 1; sq >= 0; --sq )
			for( int end = 0; end < 2; ++end )
			{
				int wrong     = 0;
				double margin = 0.0;
				if( endsCheck( W, H, sq == 1, end ? 1.0f : 0.0f, 0.0f, wrong, margin ) != 0 )
					return 1;
				const std::string label = std::string( "Squeeze " ) + ( sq ? "on,  " : "off, " ) + ( end ? "Opacity 1 (B)" : "Opacity 0 (A)" );
				//The prediction must sit a whole texel inside its sub-block,
				//or bilinear filtering could blend in the neighbour and a
				//byte comparison would be about the filter, not the lens.
				Check( margin >= 1.0, label + fmt( ": every lens's sample is %.2f texels or more inside its sub-block (needs 1)", margin ) );
				Check( wrong == 0, label + fmt( ": the card is that picture sampled at the lens pitch, all four channels (%.0f bytes differ)", wrong ) );
			}

		//Negative controls: the other Squeeze, a card at the flip, and a
		//shine on the ridges must all be told apart from the prediction.
		{
			Rig on, off;
			if( !on.Init( W, H ) || !off.Init( W, H ) )
				return 1;
			for( Rig* rig : { &on, &off } )
			{
				const double lenses = W / 20.0;
				physicsRig( *rig, lenses );
				const double F = FocalFromParam( ParamForFocal( kFocalDefault ) );
				rig->Set( "Angle Range", ParamForAngleRange( std::atan( 0.25 / F ) ) );
				rig->Set( "Opacity", 0.0f );
				const BlockCard card = blockCard( W, H, PitchFromParam( ParamForPitch( lenses ) ) );
				rig->UploadA( card.image );
				rig->UploadB( card.image );
			}
			off.Set( "Squeeze", 0.0f );
			on.Render();
			off.Render();
			Negative( differingBytes( on.Pixels(), off.Pixels() ) > 0, "Squeeze off samples other sub-blocks than Squeeze on" );
		}
		int wrongMid = 0, wrongShine = 0;
		double margin = 0.0;
		const int caughtMid = failuresOf( [ & ] {
			endsCheck( W, H, true, 0.5f, 0.0f, wrongMid, margin );
			Check( wrongMid == 0, "mid" );
		} );
		Negative( caughtMid > 0, fmt( "at Opacity 0.5 (the flip) the A-end prediction is wrong in %.0f bytes", wrongMid ) );
		const int caughtShine = failuresOf( [ & ] {
			endsCheck( W, H, true, 0.0f, 0.3f, wrongShine, margin );
			Check( wrongShine == 0, "shine" );
		} );
		Negative( caughtShine > 0, fmt( "with Ridge Shine 0.3 the ridges change %.0f bytes", wrongShine ) );
		std::printf( "\n" );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --flip: matched pitches, the viewer at infinity: the whole card flips at
// once, at the angles the lens geometry gives -- A to B square on, and back
// at the edge of the viewing zone, atan( 1 / ( 2 F ) ) either side.
//---------------------------------------------------------------------------
struct Crossing
{
	double predicted = 0.0;///< radians
	double measured  = 0.0;
};

/// m at one tilt. Returns false when a render fails. `uniform` is whether
/// every pixel of the card came back the same float, bit for bit.
bool renderM( Rig& rig, double tilt, float rangeParam, double& m, bool& uniform, double& spread, double& actualTilt )
{
	const float o = ParamForOpacity( tilt, AngleRangeFromParam( rangeParam ) );
	rig.Set( "Opacity", o );
	if( !rig.Render() )
		return false;
	const Field f = fieldOf( rig.PixelsF(), rig.width, rig.height );
	m             = f.mean;
	uniform       = f.uniformBits;
	spread        = f.hi - f.lo;
	//The tilt the Opacity the harness set stands for, through the stated
	//mapping: the float the host would send, not the plugin's own report.
	actualTilt = TiltFromParams( o, rangeParam );
	return true;
}

int flipCheck( int W, int H, int fault, const char* fragment, double focal, std::vector< Crossing >& crossings, bool& allUniform,
               double& worstSpread )
{
	Rig rig;
	if( !rig.Init( W, H, true, fault, fragment ) )
		return 1;
	physicsRig( rig );
	const double w = 0.2;//the spot, lens pitches: turns the flip into a ramp linear in tan
	rig.Set( "Focus Spot", ParamForSpot( w ) );
	rig.Set( "Focal Length", ParamForFocal( focal ) );
	const float rangeParam = ParamForAngleRange( 40.0 * kDegree );
	rig.Set( "Angle Range", rangeParam );
	const double F     = FocalFromParam( ParamForFocal( focal ) );
	const double spot  = SpotFromParam( ParamForSpot( w ) );

	crossings.clear();
	allUniform  = true;
	worstSpread = 0.0;
	//t = F tan( tilt ): the focus's offset from the lens axis, in lens
	//pitches. The strip edges are at t = -1/2, 0 and +1/2.
	for( const double tStar : { -0.5, 0.0, 0.5 } )
	{
		double m[ 2 ], t[ 2 ];
		for( int i = 0; i < 2; ++i )
		{
			const double tt = tStar + ( i ? 0.25 : -0.25 ) * spot;
			bool uniform    = false;
			double tilt     = 0.0, spread = 0.0;
			if( !renderM( rig, std::atan( tt / F ), rangeParam, m[ i ], uniform, spread, tilt ) )
				return 1;
			worstSpread = std::max( worstSpread, spread );
			t[ i ] = F * std::tan( tilt );//the tilt the Opacity set stands for
			allUniform = allUniform && uniform;
		}
		//Inside the ramp m is linear in t (a box spot over a hard strip
		//edge), so the crossing of 1/2 is where the two points' line says.
		const double tCross = t[ 0 ] + ( 0.5 - m[ 0 ] ) * ( t[ 1 ] - t[ 0 ] ) / ( m[ 1 ] - m[ 0 ] );
		crossings.push_back( { std::atan( tStar / F ), std::atan( tCross / F ) } );
	}
	return 0;
}

int runFlip()
{
	std::printf( "matched pitches, viewer at infinity: the whole card flips where the lens says\n\n" );
	//The bound. m is a closed form of about eight float operations on
	//numbers under 2 (the phase is taken relative to its own period, so the
	//lens index never rounds into it): each m is good to 8 ULP / w. The
	//crossing, from two m's on a line of slope 1 / w, is good to w * 2 * that
	//in t = F tan, so 16 ULP whatever w is; the angle to that over F, and
	//F is at least 1.2 here. The tilt reaches the shader as a float sine and
	//cosine: 2 ULP more of tan.
	const double bound = ( 16.0 / 1.2 + 2.0 ) * kFloatUlp;
	const double tol   = 1e-5;
	//Two pixels' m can differ by twice one m's 8 ULP / w, with w 0.2.
	const double mSpreadTol = 2.0 * 8.0 * kFloatUlp / 0.2;
	std::printf( "  tolerance %.0e rad; derived bound %.1e rad (asserted at most a third of it)\n", tol, bound );
	Check( 3.0 * bound <= tol, "the tolerance is at least three times the bound" );
	for( const auto& r : kRasters )
	{
		const int W = r[ 0 ], H = r[ 1 ];
		std::printf( "  %dx%d\n", W, H );
		for( const double focal : { 1.2, 1.8, 3.0 } )
		{
			std::vector< Crossing > c;
			bool uniform  = false;
			double spread = 0.0;
			if( flipCheck( W, H, kFaultNone, nullptr, focal, c, uniform, spread ) != 0 )
				return 1;
			const double F = FocalFromParam( ParamForFocal( focal ) );
			//The whole card at once: every pixel's m within the per-pixel
			//float bound of every other. Bitwise equal on this Mac's GPU, and
			//said so when it is; Apple's software renderer spreads it by an
			//ULP, and bitwise was never a promise GLSL makes.
			Check( spread <= mSpreadTol, fmt( "F %.2f: every pixel of the card has the same B fraction at every tilt measured, to %.1e (tolerance %.1e)", F,
			                                  spread, mSpreadTol )
			                                 + ( uniform ? ", bitwise" : ", not bitwise" ) );
			const char* names[ 3 ] = { "back to B below", "A to B at", "back to A above" };
			for( size_t i = 0; i < c.size(); ++i )
				Check( std::fabs( c[ i ].measured - c[ i ].predicted ) <= tol,
				       fmt( "F %.2f: ", F ) + names[ i ] + fmt( " %+.4f deg: measured %+.6f deg (off by %.1e rad)", c[ i ].predicted / kDegree,
				                                                 c[ i ].measured / kDegree, std::fabs( c[ i ].measured - c[ i ].predicted ) ) );
		}

		//Negative controls. The focus at F sin t, not F tan t: the zone's
		//edge moves by atan - asin, and the check must say so.
		const int caughtSin = failuresOf( [ & ] {
			std::vector< Crossing > c;
			bool uniform  = false;
			double spread = 0.0;
			flipCheck( W, H, kFaultSinFocal, nullptr, 1.2, c, uniform, spread );
			for( const Crossing& x : c )
				Check( std::fabs( x.measured - x.predicted ) <= tol, "sin" );
		} );
		Negative( caughtSin > 0, fmt( "the focus at F sin t instead of F tan t fails %.0f crossing(s) at F 1.2", caughtSin ) );
		//A near viewer: the card no longer flips all at once.
		{
			Rig rig;
			if( !rig.Init( W, H, true ) )
				return 1;
			physicsRig( rig );
			rig.Set( "Focus Spot", ParamForSpot( 0.2 ) );
			rig.Set( "Distance", ParamForDistance( 2.0 ) );
			rig.Render();
			const Field f = fieldOf( rig.PixelsF(), W, H );
			Negative( f.hi - f.lo > mSpreadTol, fmt( "at Distance 2 the card is not one value (%.3f .. %.3f)", f.lo, f.hi ) );
		}
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --moire: a pitch mismatch bands the flip with period
// 1 / | 1/p_L - 1/p_P | pixels, measured by FFT from the picture, and the
// bands sweep across the card as it tilts by the phase the lens predicts.
//---------------------------------------------------------------------------

/// The Hann-windowed DTFT of one row, at any frequency, in cycles per
/// picture width. Evaluated directly, so there is no bin to fall between.
struct Spectrum
{
	std::vector< double > s;///< the row, mean removed, windowed
	int W = 0;
	double windowSum = 0.0;

	explicit Spectrum( const std::vector< double >& row )
	{
		W = static_cast< int >( row.size() );
		double mean = 0.0;
		for( double v : row )
			mean += v;
		mean /= W;
		s.resize( row.size() );
		for( int x = 0; x < W; ++x )
		{
			const double h = 0.5 - 0.5 * std::cos( 2.0 * kPi * ( x + 0.5 ) / W );
			s[ static_cast< size_t >( x ) ] = ( row[ static_cast< size_t >( x ) ] - mean ) * h;
			windowSum += h;
		}
	}

	std::complex< double > At( double f ) const
	{
		std::complex< double > sum = 0.0;
		for( int x = 0; x < W; ++x )
			sum += s[ static_cast< size_t >( x ) ] * std::polar( 1.0, -2.0 * kPi * f * ( x + 0.5 ) / W );
		return sum;
	}

	/// The amplitude of a sinusoid at f: 2 |X| / sum( window ).
	double Amplitude( double f ) const
	{
		return 2.0 * std::abs( At( f ) ) / windowSum;
	}

	/// The strongest line in [lo, hi]: a grid of 1/16 cycle, then golden
	/// section on |X| to 1e-7 cycles.
	double Peak( double lo, double hi ) const
	{
		double best = lo, bestMag = -1.0;
		for( double f = lo; f <= hi; f += 1.0 / 16.0 )
		{
			const double m = std::abs( At( f ) );
			if( m > bestMag )
			{
				bestMag = m;
				best    = f;
			}
		}
		double a = best - 1.0 / 16.0, b = best + 1.0 / 16.0;
		const double g = 0.5 * ( std::sqrt( 5.0 ) - 1.0 );
		double c = b - g * ( b - a ), d = a + g * ( b - a );
		while( b - a > 1e-7 )
		{
			if( std::abs( At( c ) ) > std::abs( At( d ) ) )
				b = d;
			else
				a = c;
			c = b - g * ( b - a );
			d = a + g * ( b - a );
		}
		return 0.5 * ( a + b );
	}
};

/// The same Hann window's own transform, normalised to 1 at 0, for the
/// leakage bound. In closed form: the window is 1/2 + 1/4 e^{i..} + 1/4
/// e^{-i..} about the record's centre, so its transform is three Dirichlet
/// kernels, D( d ) = sin( pi d ) / sin( pi d / W ), and it is real. Checked
/// against the direct sum at start-up (see runMoire).
double dirichlet( int W, double delta )
{
	const double s = std::sin( kPi * delta / W );
	if( std::fabs( s ) < 1e-12 )
	{
		//delta a multiple of W: the limit, W cos( pi d ) / cos( pi d / W ).
		return W * std::cos( kPi * delta ) / std::cos( kPi * delta / W );
	}
	return std::sin( kPi * delta ) / s;
}

double hannKernel( int W, double delta )
{
	return ( 0.5 * dirichlet( W, delta ) + 0.25 * dirichlet( W, delta - 1.0 ) + 0.25 * dirichlet( W, delta + 1.0 ) ) / ( 0.5 * W );
}

double hannKernelDirect( int W, double delta )
{
	std::complex< double > sum = 0.0;
	double norm                = 0.0;
	for( int x = 0; x < W; ++x )
	{
		const double h = 0.5 - 0.5 * std::cos( 2.0 * kPi * ( x + 0.5 ) / W );
		sum += h * std::polar( 1.0, -2.0 * kPi * delta * ( x + 0.5 - 0.5 * W ) / W );
		norm += h;
	}
	return ( sum / norm ).real();
}

/// How far the picture's OTHER spectral lines can pull the measured peak
/// (cycles/width) and the measured phase (radians), from the Hann window's
/// own leakage. The lines: the print's odd harmonics j, each imaged by the
/// lens cells' hold to q N_L + j dN and folded by the pixel grid mod W. Each
/// weighs, against the fundamental, at most what a triangle-wave print (Bleed
/// 1) blurred by the box spot allows -- (1/j^2) |sinc( j w )| / |sinc( w )|
/// -- times the hold's response there against the fundamental's,
/// |sinc( q + j dN / N_L )| / |sinc( dN / N_L )|.
///
/// A line well clear of the main lobe pulls the peak by its weight times the
/// kernel's slope over the peak's curvature, and the phase by its weight
/// times the kernel. A line INSIDE the main lobe (under 1.5 cycles off)
/// merges with the fundamental, and the merged peak lies between the two, or
/// is pushed off by at most a delta / ( 1 - a ); its phase moves by at most
/// a / ( 1 - a ). Such a line must weigh under a half for either statement to
/// hold, which is asserted rather than assumed.
struct LeakBound
{
	double freq  = 0.0;
	double phase = 0.0;
	double worstNear = 0.0;///< the largest weight of a line within 1.5 cycles
};

double sincPi( double x )
{
	return std::fabs( x ) < 1e-12 ? 1.0 : std::sin( kPi * x ) / ( kPi * x );
}

LeakBound leakBound( int W, double lensPerWidth, double dN, double spotPeriods )
{
	LeakBound out;
	const double f0 = std::fabs( dN );
	const double d  = 1e-4;
	const double m0 = std::abs( hannKernel( W, 0.0 ) );
	const double curvature = std::fabs( ( std::abs( hannKernel( W, d ) ) - 2.0 * m0 + std::abs( hannKernel( W, -d ) ) ) / ( d * d ) );
	const double fund = std::fabs( sincPi( spotPeriods ) ) * std::fabs( sincPi( dN / lensPerWidth ) );
	auto fold = [ W ]( double f ) {
		double g = std::fmod( f, static_cast< double >( W ) );
		if( g < 0.0 )
			g += W;
		return g > 0.5 * W ? g - W : g;
	};
	for( int j = 1; j <= 99; j += 2 )
		for( int q = -40; q <= 40; ++q )
		{
			const double a = ( 1.0 / ( j * j ) ) * std::fabs( sincPi( j * spotPeriods ) ) * std::fabs( sincPi( q + j * dN / lensPerWidth ) ) / fund;
			for( int sign = -1; sign <= 1; sign += 2 )
			{
				if( j == 1 && q == 0 )
					continue;//the fundamental and its own negative-frequency twin, below
				const double f     = fold( sign * ( q * lensPerWidth + j * dN ) );
				const double delta = std::fabs( f - f0 );
				//A j = 1 line is the fundamental's own image: when the card
				//tilts it rotates WITH the fundamental, so it cannot bias the
				//sweep, which is a difference of two phases. Every other line
				//rotates j times as fast, and each of the two phases can be
				//off by its pull, so the difference by twice that.
				const double phaseWeight = j == 1 ? 0.0 : 2.0;
				if( delta < 1e-9 )
				{
					//On the fundamental's own frequency -- a harmonic the lens
					//sampling folds exactly onto it, as it does whenever the
					//moire is a whole number of lenses. It cannot move the
					//peak, but it rotates j times as fast.
					out.phase += phaseWeight * a;
					continue;
				}
				if( delta < 1.5 )
				{
					out.worstNear = std::max( out.worstNear, a );
					out.freq += a * delta / ( 1.0 - std::min( a, 0.99 ) );
					out.phase += phaseWeight * a / ( 1.0 - std::min( a, 0.99 ) );
					continue;
				}
				//Worst case over a cycle around the line: the kernel's slope
				//for the frequency, its magnitude for the phase.
				double slope = 0.0, mag = 0.0;
				for( double e = -0.5; e <= 0.5; e += 0.05 )
				{
					const double dd = std::max( 1.0, delta + e );
					slope = std::max( slope, std::abs( ( hannKernel( W, dd + d ) - hannKernel( W, dd - d ) ) / ( 2.0 * d ) ) );
					mag   = std::max( mag, std::abs( hannKernel( W, dd ) ) );
				}
				out.freq += a * slope / curvature;
				out.phase += phaseWeight * a * mag;
			}
		}
	//The fundamental's own twin at -f0, 2 f0 away, with weight 1; it
	//rotates the other way.
	out.freq += std::abs( ( hannKernel( W, 2.0 * f0 + d ) - hannKernel( W, 2.0 * f0 - d ) ) / ( 2.0 * d ) ) / curvature;
	out.phase += 2.0 * std::abs( hannKernel( W, 2.0 * f0 ) );
	//The row's mean removed over a record that is not a whole number of
	//bands leaves a DC residue of at most a band's worth over the record.
	const double dc = 1.0 / ( kPi * f0 );
	out.freq += dc * std::abs( ( hannKernel( W, f0 + d ) - hannKernel( W, f0 - d ) ) / ( 2.0 * d ) ) / curvature;
	out.phase += 2.0 * dc * std::abs( hannKernel( W, f0 ) );
	return out;
}

struct MoireResult
{
	double dN        = 0.0;///< N_P - N_L, signed, cycles per width
	double predictedPx = 0.0;
	double measuredF = 0.0;
	double amplitude = 0.0;
	double sweepPredicted = 0.0;///< radians
	double sweepMeasured  = 0.0;
	LeakBound bound;
};

int moireCheck( int W, int H, int fault, double lensN, double printN, MoireResult& r )
{
	Rig rig;
	if( !rig.Init( W, H, true, fault ) )
		return 1;
	physicsRig( rig );
	rig.Set( "Lens Pitch", ParamForPitch( lensN ) );
	rig.Set( "Interleave Pitch", ParamForPitch( printN ) );
	//A triangle-wave print (Bleed 1) under a wide spot: nearly a sinusoid,
	//so the lens sampling folds almost nothing back near the fundamental.
	rig.Set( "Bleed", 1.0f );
	rig.Set( "Focus Spot", ParamForSpot( 0.45 ) );
	const float rangeParam = ParamForAngleRange( 20.0 * kDegree );
	rig.Set( "Angle Range", rangeParam );
	const double range = AngleRangeFromParam( rangeParam );
	const double F     = FocalFromParam( rig.plugin.GetFloatParameter( Lenticular::PT_FOCAL_LENGTH ) );

	//The tilt each render stands for is the one the harness ASKED for, through
	//the stated Opacity mapping -- not the plugin's report of what it used,
	//which would follow a fault instead of catching it.
	auto rowAt = [ & ]( double tilt, std::vector< double >& row, lens::Setup& s ) {
		const float o = ParamForOpacity( tilt, range );
		rig.Set( "Opacity", o );
		if( !rig.Render() )
			return false;
		const ImageF px = rig.PixelsF();
		row.assign( static_cast< size_t >( W ), 0.0 );
		for( int y = 0; y < H; ++y )
			for( int x = 0; x < W; ++x )
				row[ static_cast< size_t >( x ) ] += px[ ( static_cast< size_t >( y ) * W + x ) * 4 ] / H;
		s = asked( rig.plugin );
		return true;
	};

	std::vector< double > row0, row1;
	lens::Setup s0, s1;
	if( !rowAt( 0.0, row0, s0 ) )
		return 1;
	const Spectrum spec0( row0 );
	//The pitches asked for. With the fault the plugin makes them equal and
	//there is no moire -- which is the point of the fault.
	const double nL = s0.lensPerWidth;
	const double nP = s0.printPerWidth;
	r.dN            = nP - nL;
	const double pL = W / nL, pP = W / nP;
	r.predictedPx   = 1.0 / std::fabs( 1.0 / pL - 1.0 / pP );
	r.measuredF     = spec0.Peak( 2.5, 0.5 * nL );
	r.amplitude     = spec0.Amplitude( r.measuredF );
	r.bound         = leakBound( W, nL, r.dN, SpotFromParam( ParamForSpot( 0.45 ) ) * nP / nL );

	//Tilt the card so the focus moves a tenth of a lens: every lens's phase
	//moves by a tenth of a lens in print periods, so the bands move by that
	//fraction of a band. Measured as the phase of the line at N_P - N_L.
	const double dt = 0.1;
	if( !rowAt( std::atan( dt / F ), row1, s1 ) )
		return 1;
	const Spectrum spec1( row1 );
	const double fs     = nP - nL;
	const double dPhase = std::arg( spec1.At( fs ) / spec0.At( fs ) );
	r.sweepPredicted    = std::remainder( 2.0 * kPi * s0.focal * ( std::tan( s1.tilt ) - std::tan( s0.tilt ) ) * nP / nL, 2.0 * kPi );
	r.sweepMeasured     = dPhase;
	return 0;
}

int runMoire()
{
	std::printf( "a pitch mismatch bands the flip at 1/|1/p_L - 1/p_P| pixels, measured by FFT\n\n" );
	//Tolerances, and the bounds they are checked against per case: see
	//leakBound. The float arithmetic of m (under 1e-6 per pixel, summed
	//into a Hann-weighted line) is two orders under either.
	//cycles per picture width: a fiftieth of a band across the picture,
	//against a derived worst case of 3e-3 (the fractional cases).
	const double tolF = 0.02;
	//radians of band phase: a two-hundredth of a band. The worst derived
	//bound is 9.0e-3 rad, in the whole-number case at 320 wide, where the
	//lens sampling folds the 7th and 9th harmonics exactly onto the
	//fundamental and they turn 7 and 9 times as fast when the card tilts.
	const double tolPhase = 0.03;
	struct Case
	{
		double lens, print;
		const char* what;
	};
	{
		double worst = 0.0;
		for( const int W : { 640, 320 } )
			for( double dd = 0.0; dd < 40.0; dd += 0.37 )
				worst = std::max( worst, std::fabs( hannKernel( W, dd ) - hannKernelDirect( W, dd ) ) );
		Check( worst < 1e-9, fmt( "the Hann kernel's closed form is its direct sum to %.1e", worst ) );
	}
	const Case cases[] = {
		{ 64.0, 56.0, "whole: 8 bands a width" },
		{ 64.0, 57.5, "fractional: 6.5 bands a width" },
		{ 64.0, 69.25, "print finer than the lens: 5.25 bands" },
	};
	for( const auto& rs : kRasters )
	{
		const int W = rs[ 0 ], H = rs[ 1 ];
		std::printf( "  %dx%d, a Hann-windowed DTFT of the mean row, peak by golden section\n", W, H );
		for( const Case& c : cases )
		{
			MoireResult m;
			if( moireCheck( W, H, kFaultNone, c.lens, c.print, m ) != 0 )
				return 1;
			const double measuredPx = W / m.measuredF;
			std::printf( "  %s -- N_L %.4f, N_P %.4f\n", c.what, PitchFromParam( ParamForPitch( c.lens ) ), PitchFromParam( ParamForPitch( c.print ) ) );
			Check( m.bound.worstNear < 0.5 && 3.0 * m.bound.freq <= tolF && 3.0 * m.bound.phase <= tolPhase,
			       fmt( "    the leakage bound is %.1e cycles and %.1e rad (a third of the tolerance or less); the heaviest line within 1.5 cycles weighs %.1e (needs < 0.5)", m.bound.freq,
			            m.bound.phase, m.bound.worstNear ) );
			Check( m.amplitude >= 0.1, fmt( "    there are bands: the line's amplitude is %.3f of the full 0..1 swing (needs 0.1)", m.amplitude ) );
			Check( std::fabs( m.measuredF - std::fabs( m.dN ) ) <= tolF,
			       fmt( "    the band period is %.4f px, predicted %.4f px (%.5f cycles/width off, tolerance %.2f)", measuredPx, m.predictedPx,
			            std::fabs( m.measuredF - std::fabs( m.dN ) ), tolF ) );
			Check( std::fabs( std::remainder( m.sweepMeasured - m.sweepPredicted, 2.0 * kPi ) ) <= tolPhase,
			       fmt( "    a tenth of a lens of focus moves the bands %.5f rad, predicted %.5f (tolerance %.2f)", m.sweepMeasured,
			            m.sweepPredicted, tolPhase ) );
		}
		//The negative control the spec asks for: the lens pitch forced to
		//the print's must fail at a set mismatch.
		const int caught = failuresOf( [ & ] {
			MoireResult m;
			moireCheck( W, H, kFaultPitchLocked, 64.0, 57.5, m );
			Check( m.amplitude >= 0.1, "amp" );
			Check( std::fabs( m.measuredF - 6.5 ) <= tolF, "period" );
		} );
		Negative( caught > 0, fmt( "the lens pitch forced to the print's fails %.0f assertion(s) at a 6.5-band mismatch", caught ) );
		const int caughtSweep = failuresOf( [ & ] {
			MoireResult m;
			moireCheck( W, H, kFaultTiltReversed, 64.0, 57.5, m );
			Check( std::fabs( std::remainder( m.sweepMeasured - m.sweepPredicted, 2.0 * kPi ) ) <= tolPhase, "sweep" );
		} );
		Negative( caughtSweep > 0, "the card tilted the other way sweeps the bands the other way, and the check says so" );
		std::printf( "\n" );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --distance: a viewer D picture widths away sees each column at its own
// angle, so the flip is at x = D sin T from the centre and sweeps across
// the card as it tilts. Measured by summing m across a window holding the
// one ramp: a linear functional, so a translated ramp moves the sum exactly.
//---------------------------------------------------------------------------
struct DistanceResult
{
	double predictedPx = 0.0;
	double measuredPx  = 0.0;
	double bound       = 0.0;
	bool windowOk      = false;
};

int distanceCheck( int W, int H, int fault, double D, double focal, double tiltDeg, DistanceResult& r )
{
	Rig rig;
	if( !rig.Init( W, H, true, fault ) )
		return 1;
	physicsRig( rig );
	const double w = 0.3;
	rig.Set( "Focus Spot", ParamForSpot( w ) );
	rig.Set( "Focal Length", ParamForFocal( focal ) );
	rig.Set( "Distance", ParamForDistance( D ) );
	const float rangeParam = ParamForAngleRange( 10.0 * kDegree );
	rig.Set( "Angle Range", rangeParam );
	rig.Set( "Opacity", ParamForOpacity( tiltDeg * kDegree, AngleRangeFromParam( rangeParam ) ) );
	if( !rig.Render() )
		return 1;
	const lens::Setup s = asked( rig.plugin );
	const double Dv     = 1.0 / s.invDistance;
	const double T       = s.tilt;
	const double c       = std::cos( T );
	//B left of x_b, A right of it; the ramp half a spot either side in F tan.
	const double xb       = 0.5 + Dv * std::sin( T );
	const double ramp     = 0.5 * s.spotLens * c * Dv / s.focal;//half-width, picture fractions
	const double zone     = 0.5 * c * Dv / s.focal;//the next strip edges
	const double halfWin  = 0.5 * ( ramp + ( zone - ramp ) );
	r.predictedPx         = xb * W;
	const int i0          = static_cast< int >( std::ceil( ( xb - halfWin ) * W ) );
	const int i1          = static_cast< int >( std::floor( ( xb + halfWin ) * W ) );
	r.windowOk            = i0 >= 0 && i1 <= W && halfWin > ramp && zone - ramp > halfWin;
	if( !r.windowOk )
		return 0;
	const Field f = fieldOf( rig.PixelsF(), W, H );
	double sum    = 0.0;
	for( int x = i0; x < i1; ++x )
		sum += f.m[ static_cast< size_t >( x ) ];//row 0: every row is the same card
	r.measuredPx = i0 + sum;
	//The sum of a linear ramp at pixel centres is its integral but for its
	//two kinks, each at most slope / 8 px; the ramp is 2 ramp W px wide.
	//Plus 8 ULP / w of m on each ramp pixel, and the angle's float error
	//(the tilt's sine, 1 / D and the column's offset, a few ULP of
	//tan) carried to x through D / cos T.
	const double rampPx = 2.0 * ramp * W;
	r.bound             = 2.0 * ( 1.0 / rampPx ) / 8.0 + rampPx * 8.0 * kFloatUlp / s.spotLens + 8.0 * kFloatUlp * Dv / c * W;
	return 0;
}

int runDistance()
{
	std::printf( "a viewer at distance D: the flip is at x = D sin T and sweeps across the card\n\n" );
	const double tol = 0.05;//output pixels
	struct Case
	{
		double D, focal;
		double tilts[ 5 ];
	};
	const Case cases[] = {
		{ 1.5, 1.8, { -8.0, -4.0, 0.0, 4.0, 8.0 } },
		{ 3.0, 3.0, { -4.0, -2.0, 0.0, 2.0, 4.0 } },
	};
	for( const auto& rs : kRasters )
	{
		const int W = rs[ 0 ], H = rs[ 1 ];
		std::printf( "  %dx%d, tolerance %.2f px\n", W, H, tol );
		for( const Case& c : cases )
		{
			const double Dv = 1.0 / InvDistanceFromParam( ParamForDistance( c.D ) );
			std::printf( "  D %.4f widths, F %.2f: across the card the flip angle runs from %+.3f deg (left edge) to %+.3f deg (right edge)\n", Dv,
			             c.focal, std::asin( -0.5 / Dv ) / kDegree, std::asin( 0.5 / Dv ) / kDegree );
			for( const double tilt : c.tilts )
			{
				DistanceResult r;
				if( distanceCheck( W, H, kFaultNone, c.D, c.focal, tilt, r ) != 0 )
					return 1;
				Check( r.windowOk && 3.0 * r.bound <= tol, fmt( "    tilt %+.0f deg: the window holds the one ramp and the bound %.4f px is under a third of the tolerance", tilt, r.bound ) );
				Check( std::fabs( r.measuredPx - r.predictedPx ) <= tol,
				       fmt( "    tilt %+.0f deg: the flip is at %.4f px, predicted W (1/2 + D sin T) = %.4f px (%.4f off)", tilt, r.measuredPx,
				            r.predictedPx, std::fabs( r.measuredPx - r.predictedPx ) ) );
			}
		}
		const int caught = failuresOf( [ & ] {
			for( const double tilt : { -8.0, 4.0 } )
			{
				DistanceResult r;
				distanceCheck( W, H, kFaultFlatDistance, 1.5, 1.8, tilt, r );
				Check( std::fabs( r.measuredPx - r.predictedPx ) <= tol, "flat" );
			}
		} );
		Negative( caught > 0, fmt( "Distance ignored (the viewer at infinity) fails %.0f of 2 tilts", caught ) );
		std::printf( "\n" );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --opacity: the tilt, read back out of the picture through a wide spot's
// linear ramp, rises monotonically with Opacity from -max to +max.
//---------------------------------------------------------------------------
int opacityCheck( int W, int H, int fault, std::vector< double >& measured, std::vector< double >& predicted, double& range )
{
	Rig rig;
	if( !rig.Init( W, H, true, fault ) )
		return 1;
	physicsRig( rig );
	const double w = 0.45;
	rig.Set( "Focus Spot", ParamForSpot( w ) );
	const double F = FocalFromParam( rig.plugin.GetFloatParameter( Lenticular::PT_FOCAL_LENGTH ) );
	//F tan( max ) = 0.2 of a lens, inside the ramp's +-0.225: m is linear
	//in F tan over the whole fader, and so invertible.
	const float rangeParam = ParamForAngleRange( std::atan( 0.2 / F ) );
	rig.Set( "Angle Range", rangeParam );
	range           = AngleRangeFromParam( rangeParam );
	const double spot = SpotFromParam( ParamForSpot( w ) );
	measured.clear();
	predicted.clear();
	for( int k = 0; k <= 20; ++k )
	{
		const float o = static_cast< float >( k ) / 20.0f;
		rig.Set( "Opacity", o );
		if( !rig.Render() )
			return 1;
		const Field f = fieldOf( rig.PixelsF(), W, H );
		measured.push_back( std::atan( ( f.mean - 0.5 ) * spot / F ) );
		predicted.push_back( ( 2.0 * static_cast< double >( o ) - 1.0 ) * range );
	}
	return 0;
}

int runOpacity()
{
	std::printf( "Opacity tilts the card monotonically from -Angle Range to +Angle Range\n\n" );
	//m is good to 8 ULP / w (see --flip); the tilt from it to w * that / F.
	const double bound = 8.0 * kFloatUlp * 1.0 + 2.0 * kFloatUlp;
	const double tol   = 1e-5;
	std::printf( "  tolerance %.0e rad, derived bound %.1e rad\n", tol, bound );
	Check( 3.0 * bound <= tol, "the tolerance is at least three times the bound" );
	for( const auto& rs : kRasters )
	{
		const int W = rs[ 0 ], H = rs[ 1 ];
		std::vector< double > got, want;
		double range = 0.0;
		if( opacityCheck( W, H, kFaultNone, got, want, range ) != 0 )
			return 1;
		bool monotone = true;
		double worst  = 0.0;
		for( size_t i = 0; i < got.size(); ++i )
		{
			if( i > 0 && !( got[ i ] > got[ i - 1 ] ) )
				monotone = false;
			worst = std::max( worst, std::fabs( got[ i ] - want[ i ] ) );
		}
		std::printf( "  %dx%d, range %.4f deg\n", W, H, range / kDegree );
		Check( monotone, "    the measured tilt rises strictly with Opacity, 21 steps" );
		Check( std::fabs( got.front() + range ) <= tol && std::fabs( got.back() - range ) <= tol,
		       fmt( "    Opacity 0 is %+.5f deg and 1 is %+.5f deg, predicted -+%.5f", got.front() / kDegree, got.back() / kDegree, range / kDegree ) );
		Check( worst <= tol, fmt( "    every step is (2 o - 1) max, worst %.1e rad off", worst ) );
		const int caught = failuresOf( [ & ] {
			std::vector< double > g, p;
			double rg = 0.0;
			opacityCheck( W, H, kFaultTiltReversed, g, p, rg );
			bool mono = true;
			for( size_t i = 1; i < g.size(); ++i )
				mono = mono && g[ i ] > g[ i - 1 ];
			Check( mono, "reversed" );
		} );
		Negative( caught > 0, "Opacity tilting the card the other way is not monotone rising" );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --mutation: one character of the shipped GLSL, through the plugin's own
// test hook, must fail a check; the unmutated text through the same hook
// must pass it and render the default path's bytes.
//---------------------------------------------------------------------------
int runMutation()
{
	std::printf( "one character of the shipped fragment shader fails a check\n\n" );
	const std::string shipped = kLenticularShader;
	const std::string from    = "float base = ( k + 0.5 ) * PitchRatio;";
	const std::string to      = "float base = ( k + 0.6 ) * PitchRatio;";
	const size_t at           = shipped.find( from );
	Check( at != std::string::npos && shipped.find( from, at + 1 ) == std::string::npos, "the line to mutate occurs exactly once in the shipped shader" );
	if( at == std::string::npos )
		return 1;
	std::string mutated = shipped;
	mutated.replace( at, from.size(), to );
	int differ = 0;
	for( size_t i = 0; i < shipped.size(); ++i )
		if( shipped[ i ] != mutated[ i ] )
			++differ;
	Check( differ == 1, fmt( "the mutation is one character (%.0f differ): the strip edge moves a tenth of a period off the lens axis", differ ) );

	//The default path and the hook with the shipped text render the same bytes.
	Image defaultBytes, hookBytes;
	for( int pass = 0; pass < 2; ++pass )
	{
		Rig rig;
		if( !rig.Init( 320, 180, false, kFaultNone, pass ? shipped.c_str() : nullptr ) )
			return 1;
		rig.UploadA( videoCard( 320, 180 ) );
		rig.UploadB( graphicCard( 320, 180 ) );
		rig.Set( "Opacity", 0.55f );
		rig.Set( "Lens Pitch", ParamForPitch( 40.0 ) );
		rig.Render();
		( pass ? hookBytes : defaultBytes ) = rig.Pixels();
	}
	Check( differingBytes( defaultBytes, hookBytes ) == 0, "the shipped text through the hook renders the default path's bytes" );

	const double tol = 1e-5;
	auto flipFails = [ & ]( const char* fragment ) {
		return failuresOf( [ & ] {
			std::vector< Crossing > c;
			bool uniform  = false;
			double spread = 0.0;
			flipCheck( 320, 180, kFaultNone, fragment, 1.8, c, uniform, spread );
			for( const Crossing& x : c )
				Check( std::fabs( x.measured - x.predicted ) <= tol, "crossing" );
		} );
	};
	Check( flipFails( shipped.c_str() ) == 0, "--flip passes with the shipped text through the hook" );
	const int mutatedFails = flipFails( mutated.c_str() );
	Negative( mutatedFails > 0, fmt( "--flip fails the mutated shader (%.0f of 3 crossings)", mutatedFails ) );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( int width, int height, int frames, bool moire )
{
	Rig rig;
	if( !rig.Init( width, height ) )
		return -1.0;
	rig.UploadA( videoCard( width, height ) );
	rig.UploadB( graphicCard( width, height ) );
	rig.Set( "Opacity", 0.5f );
	if( moire )
		rig.Set( "Interleave Pitch", ParamForPitch( 57.0 ) );
	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		rig.Render();
	glFinish();
	const auto start = std::chrono::steady_clock::now();
	for( int frame = 0; frame < frames; ++frame )
		rig.Render();
	glFinish();
	const auto end = std::chrono::steady_clock::now();
	return std::chrono::duration< double >( end - start ).count() * 1000.0 / frames;
}

int runBench( int frames )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "2560x1440 ", 2560, 1440 },
		{ "3840x2160 ", 3840, 2160 },
	};
	std::printf( "%d frames each, after a 20-frame warm-up, glFinish both sides. Defaults at the flip, then with a moire.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame   | moire: ms/frame   %%\n" );
	for( const Size& size : sizes )
	{
		const double ms = benchAt( size.width, size.height, frames, false );
		const double mo = benchAt( size.width, size.height, frames, true );
		if( ms < 0.0 || mo < 0.0 )
			return 1;
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%             |   %7.3f     %5.1f%%\n", size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0,
		             ms / 16.667 * 100.0, mo, mo / 16.667 * 100.0 );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --pipe: the fleet's frame format, extended to a second input, in relay's
// and wipe's shape.
//
//   * stdin is DEST, inputTextures[0], the layer below -- A. Output-sized.
//   * `--pipe-src PATH` is SRC, inputTextures[1], this layer -- B. Raw RGBA
//     frames, top row first, at `--src-size` (default: the output size).
//     PATH may be a FIFO. Without it, the `--input-b` generator is held.
//
// One frame of each is read per output frame; the run ends when either
// stream does. Lenticular has no clock, so nothing but the cues moves.
//
// `--script` is the fleet's cue sheet: one `frame Name value` per line, '#'
// to end of line a comment, the first key held before it and the last after
// it. Between keys a STANDARD parameter ramps linearly; an option, a
// boolean or an event STEPS -- it holds the earlier key's value until the
// later key's frame -- because half a Squeeze is not a thing. A name that is
// not a parameter is refused before a frame is read.
//
// SIGPIPE is ignored, so a reader that closes stdout (`| head -c 1`) makes
// the write fail and the harness exit 1, rather than dying with 141.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;

		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

/// Linear between keys for a ramping parameter; a step for the rest. The
/// first key holds before it, the last after it.
float valueAt( const Track& track, int frame, bool ramps )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame < track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			if( !ramps )
				return a.second;
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
		if( frame == track[ i ].first )
			return track[ i ].second;
	}
	return track.back().second;
}

bool readFrame( int fd, Image& frame )
{
	size_t filled = 0;
	while( filled < frame.size() )
	{
		const ssize_t got = read( fd, frame.data() + filled, frame.size() - filled );
		if( got <= 0 )
			return false;
		filled += static_cast< size_t >( got );
	}
	return true;
}

bool writeAll( int fd, const Image& frame )
{
	size_t written = 0;
	while( written < frame.size() )
	{
		const ssize_t put_ = write( fd, frame.data() + written, frame.size() - written );
		if( put_ <= 0 )
			return false;
		written += static_cast< size_t >( put_ );
	}
	return true;
}

void flipRows( const Image& in, Image& out, int width, int height )
{
	const size_t row = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( out.data() + static_cast< size_t >( height - 1 - y ) * row, in.data() + static_cast< size_t >( y ) * row, row );
}

bool applySettings( Rig& rig, const std::vector< std::string >& settings )
{
	for( const std::string& setting : settings )
	{
		const size_t equals = setting.find( '=' );
		if( equals == std::string::npos
		    || !rig.Set( setting.substr( 0, equals ), std::strtof( setting.substr( equals + 1 ).c_str(), nullptr ) ) )
		{
			std::fprintf( stderr, "--set %s: expected Name=Value with a known name (try --list)\n", setting.c_str() );
			return false;
		}
	}
	return true;
}

int runPipe( int width, int height, int srcWidth, int srcHeight, const std::string& scriptPath, const std::string& srcPath,
             const std::string& inputB, const std::vector< std::string >& settings )
{
	signal( SIGPIPE, SIG_IGN );

	Rig rig;
	if( !rig.Init( width, height, InputSpec::Exact( width, height ), InputSpec::Exact( srcWidth, srcHeight ) ) )
		return 1;
	if( !applySettings( rig, settings ) )
		return 2;

	//Resolve the script's names up front and refuse an unknown one: a
	//misspelled cue that silently did nothing would produce a take that
	//looks deliberate and is wrong.
	struct Automation
	{
		unsigned int id;
		bool ramps;
		Track track;
	};
	std::vector< Automation > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
		for( const auto& entry : tracks )
		{
			bool found = false;
			for( unsigned int id = 0; id < Lenticular::PT_ABOUT_FIRST; ++id )
			{
				const char* name = rig.plugin.GetParamName( id );
				if( name != nullptr && entry.first == name )
				{
					const unsigned int type = rig.plugin.GetParamType( id );
					automation.push_back( { id, type == FF_TYPE_STANDARD, entry.second } );
					found = true;
					break;
				}
			}
			if( !found )
			{
				std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
				return 2;
			}
		}
	}

	int srcFd = -1;
	if( !srcPath.empty() )
	{
		srcFd = open( srcPath.c_str(), O_RDONLY );
		if( srcFd < 0 )
		{
			std::fprintf( stderr, "cannot open --pipe-src %s\n", srcPath.c_str() );
			return 2;
		}
	}
	else
		rig.UploadB( generate( inputB, srcWidth, srcHeight ) );

	Image destIn( static_cast< size_t >( width ) * height * 4 ), destUp( destIn.size() );
	Image srcIn( static_cast< size_t >( srcWidth ) * srcHeight * 4 ), srcUp( srcIn.size() );
	Image out( destIn.size() );

	int index  = 0;
	int result = 0;
	for( ;; ++index )
	{
		if( !readFrame( STDIN_FILENO, destIn ) )
			break;
		if( srcFd >= 0 )
		{
			if( !readFrame( srcFd, srcIn ) )
				break;
			flipRows( srcIn, srcUp, srcWidth, srcHeight );
			rig.UploadB( srcUp );
		}
		flipRows( destIn, destUp, width, height );
		rig.UploadA( destUp );

		//Through the plugin's own setter, so a cue moves exactly what the
		//host's slider -- or, for Opacity, the layer's fader -- would.
		for( const Automation& track : automation )
			rig.plugin.SetFloatParameter( track.id, valueAt( track.track, index, track.ramps ) );

		if( !rig.Render() )
		{
			std::fprintf( stderr, "ProcessOpenGL failed at frame %d\n", index );
			result = 1;
			break;
		}
		flipRows( rig.Pixels(), out, width, height );
		if( !writeAll( STDOUT_FILENO, out ) )
		{
			std::fprintf( stderr, "lntest --pipe: stdout closed at frame %d\n", index );
			result = 1;
			break;
		}
	}

	if( srcFd >= 0 )
		close( srcFd );
	std::fprintf( stderr, "lntest --pipe: %d frames\n", index );
	return result;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"lntest -- render and measure the Lenticular FFGL mixer\n"
		"\n"
		"  --out PATH        render both inputs through the plugin (default lenticular.png)\n"
		"  --input-a NAME    the DEST generator, the layer below: A (default video)\n"
		"  --input-b NAME    the SRC generator, this layer: B (default graphic)\n"
		"                    video | graphic | quads-a | quads-b | bars | black | white | flat\n"
		"  --card PATH       write input B alone\n"
		"  --size WxH        picture size (default 1280x720)\n"
		"  --set \"Name=V\"    set a parameter by its display name, in host units. Repeatable.\n"
		"  --list            print every parameter, its kind, default and range, then exit\n"
		"  --names           no name over 16 characters or duplicated; index 0 is Squeeze\n"
		"  --mixer           two inputs, two MaxUVs, and the missing-input guards\n"
		"  --ends            tilted all the way, A or B sampled at the lens pitch, bitwise\n"
		"  --flip            the whole card flips at the angles the lens geometry predicts\n"
		"  --moire           a pitch mismatch bands at 1/|1/p_L - 1/p_P|, measured by FFT\n"
		"  --distance        a near viewer: the flip sweeps across the card as D sin T\n"
		"  --opacity         Opacity drives the tilt monotonically from -max to +max\n"
		"  --mutation        one character of the shipped GLSL fails --flip\n"
		"  --bench           time ProcessOpenGL at 720p through 4K\n"
		"  --pipe            raw RGBA Dest (A) frames on stdin, raw RGBA frames on stdout\n"
		"  --pipe-src PATH   raw RGBA Src (B) frames for --pipe (a file or FIFO); default: --input-b, held\n"
		"  --src-size WxH    the Src frames' size for --pipe (default: the output size)\n"
		"  --script PATH     parameter cues for --pipe: 'frame Name value', value in the\n"
		"                    parameter's host units (0..1); booleans step between cues,\n"
		"                    standard parameters ramp\n"
		"  --help\n" );
}

bool parseSize( const std::string& text, int& w, int& h )
{
	const size_t x = text.find( 'x' );
	if( x == std::string::npos )
		return false;
	w = std::atoi( text.substr( 0, x ).c_str() );
	h = std::atoi( text.substr( x + 1 ).c_str() );
	return w > 0 && h > 0;
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "lenticular.png";
	std::string cardPath;
	std::string inputA = "video";
	std::string inputB = "graphic";
	int width = 1280, height = 720;
	int frames = 0;
	bool wantList = false, wantBench = false, wantPipe = false;
	std::string scriptPath, srcPath;
	int srcWidth = 0, srcHeight = 0;
	std::string check;
	std::vector< std::string > settings;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--input-a" && hasNext )
			inputA = argv[ ++i ];
		else if( argument == "--input-b" && hasNext )
			inputB = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			if( !parseSize( argv[ ++i ], width, height ) )
			{
				std::fprintf( stderr, "--size wants a positive WxH\n" );
				return 2;
			}
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--pipe-src" && hasNext )
			srcPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--src-size" && hasNext )
		{
			if( !parseSize( argv[ ++i ], srcWidth, srcHeight ) )
			{
				std::fprintf( stderr, "--src-size wants a positive WxH\n" );
				return 2;
			}
		}
		else if( argument == "--names" || argument == "--mixer" || argument == "--ends" || argument == "--flip" || argument == "--moire"
		         || argument == "--distance" || argument == "--opacity" || argument == "--mutation" )
			check = argument;
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( wantList )
		return runList();
	if( check == "--names" )
		return runNames();

	if( !cardPath.empty() )
	{
		if( !writePng( cardPath, width, height, generate( inputB, width, height ) ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", cardPath.c_str() );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	int result = 0;
	if( check == "--mixer" )
		result = runMixer();
	else if( check == "--ends" )
		result = runEnds();
	else if( check == "--flip" )
		result = runFlip();
	else if( check == "--moire" )
		result = runMoire();
	else if( check == "--distance" )
		result = runDistance();
	else if( check == "--opacity" )
		result = runOpacity();
	else if( check == "--mutation" )
		result = runMutation();
	else if( wantBench )
		result = runBench( frames > 0 ? frames : 60 );
	else if( wantPipe )
		result = runPipe( width, height, srcWidth > 0 ? srcWidth : width, srcHeight > 0 ? srcHeight : height, scriptPath, srcPath, inputB,
		                  settings );
	else
	{
		Rig rig;
		if( !rig.Init( width, height ) )
			return 1;
		rig.UploadA( generate( inputA, width, height ) );
		rig.UploadB( generate( inputB, width, height ) );
		if( !applySettings( rig, settings ) )
			return 2;
		if( !rig.Render() )
		{
			std::fprintf( stderr, "ProcessOpenGL failed\n" );
			return 1;
		}
		if( !writePng( outPath, width, height, rig.Pixels() ) )
		{
			std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", outPath.c_str() );
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	if( result != 0 )
		std::printf( "\n%d check(s) FAILED\n", failures > 0 ? failures : result );
	return result;
}
