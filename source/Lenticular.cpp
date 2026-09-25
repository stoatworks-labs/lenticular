#include "Lenticular.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace lenticular;

//---------------------------------------------------------------------------
// The eighth argument is the plugin TYPE, and it is the only thing in the
// whole repo that makes this a mixer rather than an effect. The input count
// is a SEPARATE declaration, in the constructor. See genlock's AGENTS.md.
//---------------------------------------------------------------------------
static CFFGLPluginInfo PluginInfo(
	PluginFactory< Lenticular >,// Create method
	"LN01",                     // Plugin unique ID of maximum length 4.
	"SW Lenticular",            // Plugin name
	2,                          // API major version number
	1,                          // API minor version number
	0,                          // Plugin major version number
	1,                          // Plugin minor version number
	FF_MIXER,                   // Plugin type
	"A printed lenticular sheet that shows one layer or the other. The two pictures are cut into strips and interleaved under a sheet of cylindrical lenses, and the layer's opacity tilts the card: each lens focuses on the strip the viewing angle picks, so the card flips, ghosts where the focus straddles both strips, bands with moire when the print's pitch misses the lens's, sweeps across when the viewer is close, and catches the light on its ridges.\n\nThis is a MIXER: it shows this layer or the layer below.",
	"Lenticular FFGL mixer" );

static_assert( Lenticular::PT_COUNT - Lenticular::PT_ABOUT_FIRST == stoatworks::about::kParamCount,
               "the About block's size changed with the generated header" );

namespace
{
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

std::string sizeText( const FFGLTextureStruct& t )
{
	return std::to_string( t.Width ) + "x" + std::to_string( t.Height ) + " of " + std::to_string( t.HardwareWidth ) + "x"
	       + std::to_string( t.HardwareHeight );
}

/// How many Opacity changes the log records, per instance. Enough to follow a
/// whole layer transition frame by frame (a 2 s fade at 60 fps is 120), which
/// is what says what the card looked like on a transition's LAST frame; not a
/// line a frame for the length of a show.
constexpr int kOpacityLines = 400;

/// Instances are numbered in creation order, so a host session's log can tell
/// the layer's mixer from a transition's (Arena makes separate instances).
int gInstances = 0;
} // namespace

Lenticular::Lenticular()
{
	//A mixer takes exactly two. This is a separate declaration from the type
	//above, read by the host through different function codes entirely.
	SetMinInputs( 2 );
	SetMaxInputs( 2 );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//---------------------------------------------------------------------
	//Index 0, which Arena hides: on, as a real interleave is, for ever.
	params[ PT_SQUEEZE ]          = 1.0f;
	params[ PT_INTERLEAVE_PITCH ] = ParamForPitch( kPitchDefault );
	params[ PT_BLEED ]            = ParamForBleed( 0.1 );

	//The same slider position as the print's: matched, no moire, until the
	//operator detunes one.
	params[ PT_LENS_PITCH ]   = ParamForPitch( kPitchDefault );
	params[ PT_FOCAL_LENGTH ] = ParamForFocal( kFocalDefault );
	params[ PT_FOCUS_SPOT ]   = ParamForSpot( 0.1 );
	params[ PT_DISTANCE ]     = ParamForDistance( 5.0 );

	//Tilted to B: a mixer dropped on a layer at full opacity shows this
	//layer. In Resolume the layer's opacity fader overrides it from the
	//first frame.
	params[ PT_OPACITY ]     = 1.0f;
	params[ PT_ANGLE_RANGE ] = static_cast< float >( kAngleRangeDefaultDeg / kAngleRangeMaxDeg );
	params[ PT_RIDGE_SHINE ] = 0.3f;
	params[ PT_LIGHT_ANGLE ] = static_cast< float >( 0.5 + 0.5 * kLightDefaultDeg / kLightMaxDeg );

	//---------------------------------------------------------------------
	// Declaration. Every ranged parameter is a plain 0..1 float, with the
	// conversions in Controls.cpp.
	//---------------------------------------------------------------------
	//FIRST, deliberately: Resolume Arena does not expose a mixer's parameter
	//0 (measured on genlock, wipe and relay), so it holds a control whose
	//default is right if it can never be changed. See Lenticular.h.
	SetParamInfo( PT_SQUEEZE, "Squeeze", FF_TYPE_BOOLEAN, true );
	SetParamInfof( PT_INTERLEAVE_PITCH, "Interleave Pitch", FF_TYPE_STANDARD );
	SetParamInfof( PT_BLEED, "Bleed", FF_TYPE_STANDARD );

	SetParamInfof( PT_LENS_PITCH, "Lens Pitch", FF_TYPE_STANDARD );
	SetParamInfof( PT_FOCAL_LENGTH, "Focal Length", FF_TYPE_STANDARD );
	SetParamInfof( PT_FOCUS_SPOT, "Focus Spot", FF_TYPE_STANDARD );
	SetParamInfof( PT_DISTANCE, "Distance", FF_TYPE_STANDARD );

	//Named Opacity, and the name is load-bearing: Resolume binds a mixer
	//parameter called Opacity to the LAYER's opacity fader, and a layer
	//transition ramps it (measured on genlock, wipe and relay in Arena
	//7.27.1 -- writes to the mixer's own Opacity are overridden and never
	//reach the plugin). So the layer's fader tilts the card: 0 is -Angle
	//Range, A; 1 is +Angle Range, B.
	SetParamInfof( PT_OPACITY, "Opacity", FF_TYPE_STANDARD );
	SetParamInfof( PT_ANGLE_RANGE, "Angle Range", FF_TYPE_STANDARD );
	SetParamInfof( PT_RIDGE_SHINE, "Ridge Shine", FF_TYPE_STANDARD );
	SetParamInfof( PT_LIGHT_ANGLE, "Light Angle", FF_TYPE_STANDARD );

	// Groups, the way Resolume shows them: each group one contiguous run.
	for( FFUInt32 i = PT_SQUEEZE; i <= PT_BLEED; ++i )
		SetParamGroup( i, "Print" );
	for( FFUInt32 i = PT_LENS_PITCH; i <= PT_DISTANCE; ++i )
		SetParamGroup( i, "Lens" );
	for( FFUInt32 i = PT_OPACITY; i <= PT_LIGHT_ANGLE; ++i )
		SetParamGroup( i, "View" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Lenticular mixer" );

	diag::init();
	instance = ++gInstances;
}

//---------------------------------------------------------------------------
FFResult Lenticular::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !shader.Compile( kVertexShader, fragmentOverride ? fragmentOverride : kLenticularShader ) )
	{
		//Returning FF_FAIL here is invisible to the operator: the mixer
		//simply does nothing in Resolume. These lines are the only record.
		diag::error( "the lenticular shader failed to compile - the mixer will do nothing" );
		FFGLLog::LogToHost( "Lenticular: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised, viewport " + std::to_string( vp ? vp->width : 0 ) + "x" + std::to_string( vp ? vp->height : 0 )
	            + " (instance #" + std::to_string( instance ) + ")" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

void Lenticular::LogGuard( unsigned bit, const char* what )
{
	if( guardsLogged & bit )
		return;
	guardsLogged |= bit;
	diag::warn( std::string( "guard: called with " ) + what + " -- returned FF_FAIL (logged once)" );
}

//---------------------------------------------------------------------------
FFResult Lenticular::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	//The SDK's Add example guards on the input count and on each pointer,
	//with a comment saying a host calls a mixer with one input while the
	//operator is still patching. Not observed in Arena (genlock, one
	//sequence) -- but a mixer that dereferenced a null would take Resolume
	//down with it.
	if( pGL == nullptr || pGL->inputTextures == nullptr )
	{
		LogGuard( 1u, "no input array" );
		return FF_FAIL;
	}
	if( pGL->numInputTextures < 2 )
	{
		LogGuard( 2u, ( std::to_string( pGL->numInputTextures ) + " input(s)" ).c_str() );
		return FF_FAIL;
	}
	if( pGL->inputTextures[ 0 ] == nullptr || pGL->inputTextures[ 1 ] == nullptr )
	{
		LogGuard( 4u, "a null input" );
		return FF_FAIL;
	}

	const FFGLTextureStruct& a = *pGL->inputTextures[ 0 ];//the layer below
	const FFGLTextureStruct& b = *pGL->inputTextures[ 1 ];//this layer
	//HardwareWidth and HardwareHeight are the DENOMINATORS in
	//GetMaxGLTexCoords, so a zero there is an infinite MaxUV.
	if( a.Width == 0 || a.Height == 0 || a.HardwareWidth == 0 || a.HardwareHeight == 0 || b.Width == 0 || b.Height == 0
	    || b.HardwareWidth == 0 || b.HardwareHeight == 0 )
	{
		LogGuard( 8u, "a zero-sized input" );
		return FF_FAIL;
	}

	const int outW = static_cast< int >( currentViewport.width );
	const int outH = static_cast< int >( currentViewport.height );
	if( outW <= 0 || outH <= 0 )
		return FF_FAIL;

	++frames;
	if( frames == 1 )
		diag::info( "first frame: A " + sizeText( a ) + ", B " + sizeText( b ) + ", out " + std::to_string( outW ) + "x"
		            + std::to_string( outH ) );
	if( params[ PT_OPACITY ] != loggedOpacity && opacityLines < kOpacityLines )
	{
		//The only evidence a host gives of what drives a mixer's Opacity:
		//the layer fader, a transition, or the mixer's own slider.
		diag::info( "Opacity " + std::to_string( params[ PT_OPACITY ] ) + " at frame " + std::to_string( frames )
		            + " (instance #" + std::to_string( instance ) + ")" );
		loggedOpacity = params[ PT_OPACITY ];
		++opacityLines;
	}

	//-----------------------------------------------------------------
	// The card, as numbers. In double here; the shader gets floats.
	//-----------------------------------------------------------------
	lens::Setup s;
	s.squeeze       = params[ PT_SQUEEZE ] > 0.5f;
	s.printPerWidth = PitchFromParam( params[ PT_INTERLEAVE_PITCH ] );
	s.lensPerWidth  = ( fault & kFaultPitchLocked ) ? s.printPerWidth : PitchFromParam( params[ PT_LENS_PITCH ] );
	//Exactly 1.0 when the two sliders agree: the same float through the same
	//pow is the same double.
	s.ratio         = s.printPerWidth / s.lensPerWidth;
	s.focal         = FocalFromParam( params[ PT_FOCAL_LENGTH ] );
	s.spotLens      = SpotFromParam( params[ PT_FOCUS_SPOT ] );
	s.spotPeriods   = std::min( s.spotLens * s.ratio, lens::kSpotPeriodsMax );
	s.bleedPeriods  = 0.5 * BleedFromParam( params[ PT_BLEED ] );//a strip is half a period
	s.invDistance   = InvDistanceFromParam( params[ PT_DISTANCE ] );
	s.tilt          = TiltFromParams( params[ PT_OPACITY ], params[ PT_ANGLE_RANGE ] );
	if( fault & kFaultTiltReversed )
		s.tilt = -s.tilt;
	s.shine   = std::clamp( static_cast< double >( params[ PT_RIDGE_SHINE ] ), 0.0, 1.0 );
	s.light   = LightFromParam( params[ PT_LIGHT_ANGLE ] );
	s.distort = lens::kDistortMax * s.shine;
	lastSetup = s;

	const double degree         = kPi / 180.0;
	const double highlightWidth = std::max( lens::kHighlightWidth, lens::kHighlightMinPx * s.lensPerWidth / outW );

	//-----------------------------------------------------------------
	// Bind both inputs. The declaration ORDER matters: every
	// ffglex::Scoped* clears its binding on exit rather than restoring it,
	// so they must unwind as activate(1), bind(1) then activate(0), bind(0).
	//-----------------------------------------------------------------
	ScopedShaderBinding shaderBinding( shader.GetGLID() );
	ScopedSamplerActivation activateA( 0 );
	Scoped2DTextureBinding bindA( a.Handle );
	ScopedSamplerActivation activateB( 1 );
	Scoped2DTextureBinding bindB( b.Handle );

	shader.Set( "TextureA", 0 );
	shader.Set( "TextureB", 1 );

	//One MaxUV per input. They are different numbers whenever the two
	//layers are different sizes, which for a mixer is the normal case.
	const FFGLTexCoords maxA = GetMaxGLTexCoords( a );
	const FFGLTexCoords maxB = GetMaxGLTexCoords( b );
	shader.Set( "MaxUVA", maxA.s, maxA.t );
	shader.Set( "MaxUVB", maxB.s, maxB.t );
	shader.Set( "HalfTexelA", 0.5f / static_cast< float >( a.Width ), 0.5f / static_cast< float >( a.Height ) );
	shader.Set( "HalfTexelB", 0.5f / static_cast< float >( b.Width ), 0.5f / static_cast< float >( b.Height ) );
	glUniform2i( shader.FindUniform( "OutSize" ), outW, outH );

	shader.Set( "LensPerWidth", static_cast< float >( s.lensPerWidth ) );
	shader.Set( "PrintPerWidth", static_cast< float >( s.printPerWidth ) );
	shader.Set( "PitchRatio", static_cast< float >( s.ratio ) );
	shader.Set( "Focal", static_cast< float >( s.focal ) );
	shader.Set( "SpotPeriods", static_cast< float >( s.spotPeriods ) );
	shader.Set( "BleedPeriods", static_cast< float >( s.bleedPeriods ) );
	shader.Set( "Squeeze", s.squeeze ? 1 : 0 );

	shader.Set( "SinTilt", static_cast< float >( std::sin( s.tilt ) ) );
	shader.Set( "CosTilt", static_cast< float >( std::cos( s.tilt ) ) );
	shader.Set( "InvDistance", static_cast< float >( ( fault & kFaultFlatDistance ) ? 0.0 : s.invDistance ) );

	shader.Set( "Shine", static_cast< float >( s.shine ) );
	shader.Set( "LightAngle", static_cast< float >( s.light ) );
	shader.Set( "Distort", static_cast< float >( s.distort ) );
	shader.Set( "RidgeSlope", static_cast< float >( std::sin( lens::kRidgeEdgeSlopeDeg * degree ) ) );
	shader.Set( "HighlightWidth", static_cast< float >( highlightWidth ) );
	shader.Set( "EdgeShade", static_cast< float >( lens::kEdgeShade ) );

	shader.Set( "Fault", fault );

	quad.Draw();

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Lenticular::DeInitGL()
{
	if( frames > 0 )
		diag::info( "DeInitGL after " + std::to_string( frames ) + " frames, last Opacity " + std::to_string( params[ PT_OPACITY ] )
		            + " (instance #" + std::to_string( instance ) + ")" );
	shader.FreeGLResources();
	quad.Release();
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Lenticular::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Lenticular::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Lenticular::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Lenticular::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

//---------------------------------------------------------------------------
void Lenticular::SetHostInfo( const char* hostname, const char* version )
{
	CFFGLPlugin::SetHostInfo( hostname, version );
	diag::info( std::string( "host=" ) + ( hostname ? hostname : "?" ) + " version=" + ( version ? version : "?" )
	            + " loaded from " + diag::modulePath() );
}
