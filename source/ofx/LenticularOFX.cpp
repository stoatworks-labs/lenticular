/// The OpenFX build of Lenticular, as a **Transition**: DaVinci Resolve, Vegas
/// and any other host with an OpenFX Transition context.
///
/// ------------------------------------------------------- the one mapping
///
/// The FFGL build is a mixer: the layer below is A, this layer is B, and the
/// layer's opacity fader -- a parameter named Opacity, which Resolume binds
/// to the fader and ramps in a transition -- tilts the card from A at 0 to B
/// at 1. An OpenFX transition is the same shape with the host's names on it:
///
///     FFGL                                  OpenFX Transition context
///     inputTextures[ 0 ], the layer below   SourceFrom   A, the first strip
///     inputTextures[ 1 ], this layer        SourceTo     B, the second strip
///     Opacity, the layer fader              Transition   the host's 0 -> 1
///
/// So the host's own transition progress tilts the card, and nothing else
/// about the model moves. The Transition parameter is the host's: in this
/// context the plugin may describe it and read it, and nothing else -- no
/// label, no range, no page.
///
/// ------------------------------------------------------- what is shared
///
/// **Everything but the pixel loop's body, and that is a twin.** Card.h is the
/// FFGL build's own GL-free code, linked straight in: the 0..1 parameter
/// mappings (Controls.cpp), the defaults, the lens setup and the uniforms --
/// the same functions ProcessOpenGL hands the GPU, so the two builds are handed
/// the same floats. The shader's per-pixel stage, which the GPU runs, is
/// transcribed line for line in Card.cpp (`card::Shade`, marked `//= mirrored`
/// on both sides), sampling included: GL_LINEAR at a position clamped half a
/// texel inside the picture. `lntest --cpu` compares it with the real FFGL
/// plugin's GPU render per pixel. This file only marshals pixels.
///
/// ------------------------------------------------------- what is not here
///
/// **Opacity, as a parameter.** Its job is the host's Transition parameter.
///
/// **The Filter and General contexts.** A one-input Filter has no B to flip
/// to. General would be a few lines, but it is a different product: a
/// two-input node whose tilt nobody drives, because the host owns Transition
/// only in the Transition context -- the plugin would have to declare its own
/// tilt slider, labelled and ranged, for the operator to keyframe. And in a
/// host that lists General-context plugins among its clip effects as well as
/// its transitions, Resolve among them, it would put a second "Lenticular"
/// where one clip is connected and there is nothing to flip to. Not declared,
/// so hosts with no Transition context (Nuke) do not list this build at all;
/// that is a known gap, not an accident.
///
/// Nothing else is missing. The FFGL build has no audio, no clock, no buffer
/// and no presets, so there was nothing to drop or reformulate: every frame is
/// a pure function of the two inputs at that time and the parameters at that
/// time, which is exactly what a host that renders frames out of order, alone
/// and concurrently needs.
///
/// ------------------------------------------------------- inherited, on purpose
///
/// **The lens has no end stops.** Transition 0 is the card tilted to A *through
/// the lens* -- stepped at the lens pitch, ridges and all -- not the plain
/// SourceFrom clip, so a transition opens with a cut to the card and closes
/// with a cut from it. Resolume shows the same pop at a transition's end. It
/// is the effect as designed, and the plugin description says so.
///
/// ------------------------------------------------------- tiles
///
/// Declined. A lens samples the print at a position the tilt chose, up to a
/// whole interleave period from the pixel it lands in, so there is no tile of
/// the inputs smaller than the frame that an output tile can be honestly
/// computed from.

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Card.h"
#include "../Controls.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.lenticular";
constexpr const char* kPluginName       = "Lenticular";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"A printed lenticular sheet that shows one clip or the other.\n\n"
	"The two pictures are cut into strips and interleaved under a sheet of "
	"cylindrical lenses, and the transition tilts the card: each lens focuses "
	"on the strip the viewing angle picks, so the card flips, ghosts where the "
	"focus straddles both strips, bands with moire when the print's pitch "
	"misses the lens's, sweeps across when the viewer is close, and catches "
	"the light on its ridges.\n\n"
	"SourceFrom is printed in each lens's first strip and SourceTo in the "
	"second; the transition's progress is the tilt, from -Angle Range at the "
	"start to +Angle Range at the end. The lens has no end stops, so the first "
	"and last frames are the card, not the plain clips: the transition cuts "
	"to the card and from it. The Resolume build does the same.\n\n"
	"https://stoatworks-labs.com";

// Script names. A saved project refers to these: never rename one.
constexpr const char* kParamSqueeze         = "squeeze";
constexpr const char* kParamInterleavePitch = "interleavePitch";
constexpr const char* kParamBleed           = "bleed";
constexpr const char* kParamLensPitch       = "lensPitch";
constexpr const char* kParamFocalLength     = "focalLength";
constexpr const char* kParamFocusSpot       = "focusSpot";
constexpr const char* kParamDistance        = "distance";
constexpr const char* kParamAngleRange      = "angleRange";
constexpr const char* kParamRidgeShine      = "ridgeShine";
constexpr const char* kParamLightAngle      = "lightAngle";

using namespace lenticular;

//---------------------------------------------------------------------------
// One input, gathered: premultiplied float RGBA, row 0 at the bottom --
// OpenFX's orientation and the GL's, so nothing here flips. The card's
// sampler reads this exactly as the GPU reads a texture.
//---------------------------------------------------------------------------
struct Plane
{
	std::vector< float > rgba;
	int width  = 0;
	int height = 0;
};

template< typename Pixel, int Components, int Maximum >
void gatherRows( const OFX::Image& src, const OfxRectI& bounds, bool premultiplied, Plane& out, int y0, int y1 )
{
	const float scale = 1.0f / static_cast< float >( Maximum );

	for( int y = y0; y < y1; ++y )
	{
		float* row = out.rgba.data() + static_cast< size_t >( y ) * static_cast< size_t >( out.width ) * 4;
		for( int x = 0; x < out.width; ++x )
		{
			const Pixel* px = static_cast< const Pixel* >( src.getPixelAddress( bounds.x1 + x, bounds.y1 + y ) );
			float* dst      = row + static_cast< size_t >( x ) * 4;
			if( px == nullptr )
			{
				dst[ 0 ] = dst[ 1 ] = dst[ 2 ] = dst[ 3 ] = 0.0f;
				continue;
			}

			const float a = Components == 4 ? static_cast< float >( px[ 3 ] ) * scale : 1.0f;
			for( int c = 0; c < 3; ++c )
			{
				const float v = static_cast< float >( px[ c ] ) * scale;
				//The card works premultiplied, as the FFGL build assumes its
				//textures are: a fetch between two texels and the ridge's
				//white composited OVER both mean what they say only there.
				dst[ c ] = premultiplied ? v : v * a;
			}
			dst[ 3 ] = a;
		}
	}
}

/// Converts one input on every thread the host offers, a band of rows each.
class GatherProcessor : public OFX::MultiThread::Processor
{
public:
	GatherProcessor( const OFX::Image& srcValue, bool premultipliedValue, Plane& outValue ) :
		src( srcValue ),
		premultiplied( premultipliedValue ),
		out( outValue )
	{
		bounds = src.getBounds();
	}

	void multiThreadFunction( unsigned int threadId, unsigned int nThreads ) override
	{
		const int rows  = out.height;
		const int chunk = std::max( 1, static_cast< int >( ( rows + static_cast< int >( nThreads ) - 1 ) / static_cast< int >( nThreads ) ) );
		const int y0    = static_cast< int >( threadId ) * chunk;
		const int y1    = std::min( rows, y0 + chunk );
		if( y0 >= y1 )
			return;

		const OFX::BitDepthEnum depth       = src.getPixelDepth();
		const OFX::PixelComponentEnum comps = src.getPixelComponents();
		const bool rgba                     = comps == OFX::ePixelComponentRGBA;

		switch( depth )
		{
		case OFX::eBitDepthUByte:
			rgba ? gatherRows< unsigned char, 4, 255 >( src, bounds, premultiplied, out, y0, y1 )
			     : gatherRows< unsigned char, 3, 255 >( src, bounds, premultiplied, out, y0, y1 );
			break;
		case OFX::eBitDepthUShort:
			rgba ? gatherRows< unsigned short, 4, 65535 >( src, bounds, premultiplied, out, y0, y1 )
			     : gatherRows< unsigned short, 3, 65535 >( src, bounds, premultiplied, out, y0, y1 );
			break;
		case OFX::eBitDepthFloat:
			rgba ? gatherRows< float, 4, 1 >( src, bounds, premultiplied, out, y0, y1 )
			     : gatherRows< float, 3, 1 >( src, bounds, premultiplied, out, y0, y1 );
			break;
		default:
			break;//refused before the gather starts; see gather()
		}
	}

private:
	const OFX::Image& src;
	const bool premultiplied;
	Plane& out;
	OfxRectI bounds {};
};

bool supported( const OFX::Image& image )
{
	const OFX::BitDepthEnum depth       = image.getPixelDepth();
	const OFX::PixelComponentEnum comps = image.getPixelComponents();
	return ( depth == OFX::eBitDepthUByte || depth == OFX::eBitDepthUShort || depth == OFX::eBitDepthFloat )
	       && ( comps == OFX::ePixelComponentRGBA || comps == OFX::ePixelComponentRGB );
}

/// One input into a Plane. A missing input -- a host is entitled to have no
/// frame on one side -- is a single transparent texel, which the clamped
/// fetch then returns for every position: the card shows nothing on that
/// side rather than failing the render.
void gather( const OFX::Image* src, Plane& out )
{
	if( src == nullptr )
	{
		out.width  = 1;
		out.height = 1;
		out.rgba.assign( 4, 0.0f );
		return;
	}
	if( !supported( *src ) )
		OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

	const OfxRectI bounds = src->getBounds();
	out.width             = std::max( 0, bounds.x2 - bounds.x1 );
	out.height            = std::max( 0, bounds.y2 - bounds.y1 );
	if( out.width == 0 || out.height == 0 )
	{
		gather( nullptr, out );
		return;
	}
	out.rgba.assign( static_cast< size_t >( out.width ) * static_cast< size_t >( out.height ) * 4, 0.0f );

	//An RGB clip has no alpha to be premultiplied by, and a host that says
	//"unpremultiplied" about one is describing something that does not exist.
	const bool premultiplied = src->getPixelComponents() != OFX::ePixelComponentRGBA
	                           || src->getPreMultiplication() != OFX::eImageUnPreMultiplied;

	GatherProcessor processor( *src, premultiplied, out );
	processor.multiThread();
}

//---------------------------------------------------------------------------
// The card, into the host's buffer. One call of card::Shade per pixel of the
// render window, on every thread the host offers. Each output pixel is a
// function of the inputs and the frame's uniforms alone, so the split into
// rows changes nothing.
//---------------------------------------------------------------------------
struct Frame
{
	card::Uniforms uniforms;
	card::Texture a, b;
	bool premultipliedOut = true;
};

class CardProcessorBase : public OFX::ImageProcessor
{
public:
	explicit CardProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setFrame( const Frame* value )
	{
		frame = value;
	}

protected:
	const Frame* frame = nullptr;
};

template< class PIX, int nComponents, int maxValue >
class CardProcessor : public CardProcessorBase
{
public:
	explicit CardProcessor( OFX::ImageEffect& effect ) :
		CardProcessorBase( effect )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const Frame& f        = *frame;
		const OfxRectI bounds = _dstImg->getBounds();

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dstPix = static_cast< PIX* >( _dstImg->getPixelAddress( window.x1, y ) );
			if( dstPix == nullptr )
				continue;

			for( int x = window.x1; x < window.x2; ++x, dstPix += nComponents )
			{
				float out[ 4 ];
				//The picture is the output's bounds: pixel (0, 0) is its
				//bottom-left, as the shader's floor( uv * OutSize ) is.
				card::Shade( f.uniforms, f.a, f.b, x - bounds.x1, y - bounds.y1, out );

				const float alpha = out[ 3 ];
				for( int c = 0; c < 3; ++c )
				{
					float v = out[ c ];
					if( !f.premultipliedOut )
						v = alpha > 0.0f ? v / alpha : 0.0f;
					dstPix[ c ] = fromFloat( v );
				}
				if( nComponents == 4 )
					dstPix[ 3 ] = fromFloat( alpha );
			}
		}
	}

private:
	static PIX fromFloat( float value )
	{
		//Float buffers are left alone: a host working in float may carry
		//values outside 0..1 in its inputs, and the card only ever mixes them.
		if( maxValue == 1 )
			return static_cast< PIX >( value );
		//Integer buffers round to nearest, as the GL converts a fragment to
		//an RGBA8 framebuffer.
		const float clamped = std::min( std::max( value, 0.0f ), 1.0f );
		return static_cast< PIX >( std::lround( clamped * static_cast< float >( maxValue ) ) );
	}
};

//---------------------------------------------------------------------------
class LenticularOFXPlugin : public OFX::ImageEffect
{
public:
	explicit LenticularOFXPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip  = fetchClip( kOfxImageEffectOutputClipName );
		fromClip = fetchClip( kOfxImageEffectTransitionSourceFromClipName );
		toClip   = fetchClip( kOfxImageEffectTransitionSourceToClipName );

		transition      = fetchDoubleParam( kOfxImageEffectTransitionParamName );
		squeeze         = fetchBooleanParam( kParamSqueeze );
		interleavePitch = fetchDoubleParam( kParamInterleavePitch );
		bleed           = fetchDoubleParam( kParamBleed );
		lensPitch       = fetchDoubleParam( kParamLensPitch );
		focalLength     = fetchDoubleParam( kParamFocalLength );
		focusSpot       = fetchDoubleParam( kParamFocusSpot );
		distance        = fetchDoubleParam( kParamDistance );
		angleRange      = fetchDoubleParam( kParamAngleRange );
		ridgeShine      = fetchDoubleParam( kParamRidgeShine );
		lightAngle      = fetchDoubleParam( kParamLightAngle );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		if( dst == nullptr )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		//Both are mandated in this context, but "connected" and "has a frame
		//at this time" are different promises. See gather().
		std::unique_ptr< OFX::Image > from( fromClip->isConnected() ? fromClip->fetchImage( args.time ) : nullptr );
		std::unique_ptr< OFX::Image > to( toClip->isConnected() ? toClip->fetchImage( args.time ) : nullptr );

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();
		if( !supported( *dst ) )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		const OfxRectI bounds = dst->getBounds();
		const int outW        = bounds.x2 - bounds.x1;
		const int outH        = bounds.y2 - bounds.y1;
		if( outW <= 0 || outH <= 0 )
			return;

		//Every parameter at this frame's time, once, on this thread. Nothing
		//is read during the pixel loop and nothing is kept for the next frame.
		const card::HostValues values = valuesAt( args.time );

		Frame frame;
		frame.uniforms = card::UniformsFor( card::SetupFor( values ), outW, outH );

		Plane planeA, planeB;
		gather( from.get(), planeA );
		gather( to.get(), planeB );
		frame.a = card::MakeTexture( planeA.rgba.data(), planeA.width, planeA.height );
		frame.b = card::MakeTexture( planeB.rgba.data(), planeB.width, planeB.height );

		frame.premultipliedOut = comps != OFX::ePixelComponentRGBA || dst->getPreMultiplication() != OFX::eImageUnPreMultiplied;

		const bool rgba = comps == OFX::ePixelComponentRGBA;
		switch( depth )
		{
		case OFX::eBitDepthUByte:
			rgba ? run< CardProcessor< unsigned char, 4, 255 > >( args, dst.get(), frame )
			     : run< CardProcessor< unsigned char, 3, 255 > >( args, dst.get(), frame );
			break;
		case OFX::eBitDepthUShort:
			rgba ? run< CardProcessor< unsigned short, 4, 65535 > >( args, dst.get(), frame )
			     : run< CardProcessor< unsigned short, 3, 65535 > >( args, dst.get(), frame );
			break;
		case OFX::eBitDepthFloat:
			rgba ? run< CardProcessor< float, 4, 1 > >( args, dst.get(), frame )
			     : run< CardProcessor< float, 3, 1 > >( args, dst.get(), frame );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		if( stoatworks::about::ofx::changedParam( args, paramName ) )
			return;
	}

private:
	template< class Processor >
	void run( const OFX::RenderArguments& args, OFX::Image* dst, const Frame& frame )
	{
		Processor processor( *this );
		processor.setDstImg( dst );
		processor.setFrame( &frame );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	/// The FFGL build's 0..1 host values, from this host's parameters at time
	/// t. Doubles become floats here, as FFGL's are: equal sliders are then
	/// equal floats, so matched pitches are matched bit for bit here too.
	card::HostValues valuesAt( double t ) const
	{
		const auto at = [ t ]( OFX::DoubleParam* p ) { return static_cast< float >( p->getValueAtTime( t ) ); };

		card::HostValues v;
		v.squeeze         = squeeze->getValueAtTime( t ) ? 1.0f : 0.0f;
		v.interleavePitch = at( interleavePitch );
		v.bleed           = at( bleed );
		v.lensPitch       = at( lensPitch );
		v.focalLength     = at( focalLength );
		v.focusSpot       = at( focusSpot );
		v.distance        = at( distance );
		v.opacity         = at( transition );//the tilt: FFGL's Opacity
		v.angleRange      = at( angleRange );
		v.ridgeShine      = at( ridgeShine );
		v.lightAngle      = at( lightAngle );
		return v;
	}

	OFX::Clip* dstClip  = nullptr;
	OFX::Clip* fromClip = nullptr;
	OFX::Clip* toClip   = nullptr;

	OFX::DoubleParam* transition      = nullptr;
	OFX::BooleanParam* squeeze        = nullptr;
	OFX::DoubleParam* interleavePitch = nullptr;
	OFX::DoubleParam* bleed           = nullptr;
	OFX::DoubleParam* lensPitch       = nullptr;
	OFX::DoubleParam* focalLength     = nullptr;
	OFX::DoubleParam* focusSpot       = nullptr;
	OFX::DoubleParam* distance        = nullptr;
	OFX::DoubleParam* angleRange      = nullptr;
	OFX::DoubleParam* ridgeShine      = nullptr;
	OFX::DoubleParam* lightAngle      = nullptr;
};

//---------------------------------------------------------------------------
OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                          OFX::GroupParamDescriptor* group, const char* name, const char* label,
                                          const char* hint, float value )
{
	OFX::DoubleParamDescriptor* param = desc.defineDoubleParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setRange( 0.0, 1.0 );
	param->setDisplayRange( 0.0, 1.0 );
	param->setDefault( static_cast< double >( value ) );
	param->setIncrement( 0.001 );
	param->setDoubleType( OFX::eDoubleTypePlain );
	param->setParent( *group );
	page->addChild( *param );
	return param;
}

OFX::GroupParamDescriptor* defineGroup( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, const char* name )
{
	OFX::GroupParamDescriptor* group = desc.defineGroupParam( name );
	group->setLabels( name, name, name );
	page->addChild( *group );
	return group;
}

mDeclarePluginFactory( LenticularPluginFactory, {}, {} );
} // namespace

void LenticularPluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	// The Transition context only: see the note at the top of this file.
	desc.addSupportedContext( OFX::eContextTransition );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// A lens fetches the print up to a whole interleave period away from the
	// pixel it lands in, so no input tile smaller than the frame will do.
	// Frames stay independent of each other and of render order: there is no
	// clock and no history to keep.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setSupportsMultipleClipPARs( false );
	desc.setSupportsMultipleClipDepths( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void LenticularPluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	// The mandated clips. SourceFrom is A, printed in each lens's first strip:
	// FFGL's layer below. SourceTo is B, in the second: FFGL's own layer.
	for( const char* name : { kOfxImageEffectTransitionSourceFromClipName, kOfxImageEffectTransitionSourceToClipName } )
	{
		OFX::ClipDescriptor* clip = desc.defineClip( name );
		clip->addSupportedComponent( OFX::ePixelComponentRGBA );
		clip->addSupportedComponent( OFX::ePixelComponentRGB );
		clip->setTemporalClipAccess( false );
		clip->setSupportsTiles( false );
	}

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	// The mandated Transition parameter, which carries the tilt. Described and
	// nothing more: in this context the host owns its label, range and place,
	// and drives it 0 -> 1 across the transition.
	desc.defineDoubleParam( kOfxImageEffectTransitionParamName );

	// Same parameters, same 0..1 ranges, same defaults and the same groups as
	// the FFGL build -- the defaults are read from the same struct -- so one set
	// of docs covers both and a value means the same thing in each.
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );
	const card::HostValues defaults;

	//------------------------------------------------------------------ Print
	OFX::GroupParamDescriptor* print = defineGroup( desc, page, "Print" );

	OFX::BooleanParamDescriptor* squeezeParam = desc.defineBooleanParam( kParamSqueeze );
	squeezeParam->setLabels( "Squeeze", "Squeeze", "Squeeze" );
	squeezeParam->setHint( "On: each strip holds its picture squeezed, as a real interleave does -- the A strip "
	                       "of a period holds A's band of one period compressed into half of one. Off: each strip "
	                       "is a window on the picture where it is." );
	squeezeParam->setDefault( defaults.squeeze > 0.5f );
	squeezeParam->setParent( *print );
	page->addChild( *squeezeParam );

	defineSlider( desc, page, print, kParamInterleavePitch, "Interleave Pitch",
	              "The print's periods -- one A strip and one B strip -- across the picture width, 4 to 480, "
	              "geometric. On the same scale as Lens Pitch: equal sliders are a matched print.",
	              defaults.interleavePitch );
	defineSlider( desc, page, print, kParamBleed, "Bleed",
	              "The ramp between two strips on the print, as a fraction of a strip. At 1 the print is a "
	              "triangle wave.",
	              defaults.bleed );

	//------------------------------------------------------------------- Lens
	OFX::GroupParamDescriptor* lens = defineGroup( desc, page, "Lens" );

	defineSlider( desc, page, lens, kParamLensPitch, "Lens Pitch",
	              "Lenses across the picture width, 4 to 480, on the same scale as Interleave Pitch. Detune "
	              "one from the other and the card flips in moire bands of 1 / |1/p_L - 1/p_P|.",
	              defaults.lensPitch );
	defineSlider( desc, page, lens, kParamFocalLength, "Focal Length",
	              "0.5 to 6 lens pitches, geometric. The viewing zone -- the tilt across which one strip is "
	              "seen -- is atan( 1 / 2F ) either side of square on.",
	              defaults.focalLength );
	defineSlider( desc, page, lens, kParamFocusSpot, "Focus Spot",
	              "The width of the lens's focus on the print, up to half a lens: the aberration that makes "
	              "the ghost of both pictures near the flip.",
	              defaults.focusSpot );
	defineSlider( desc, page, lens, kParamDistance, "Distance",
	              "The viewer, from half a picture width away at 0 to infinity at 1. Linear in 1 / distance. "
	              "A near viewer sees each column at its own angle, so the flip sweeps across the card.",
	              defaults.distance );

	//------------------------------------------------------------------- View
	OFX::GroupParamDescriptor* view = defineGroup( desc, page, "View" );

	defineSlider( desc, page, view, kParamAngleRange, "Angle Range",
	              "How far the transition tilts the card: from -range at its start to +range at its end, "
	              "0 to 45 degrees. Halfway through, the card is square on.",
	              defaults.angleRange );
	defineSlider( desc, page, view, kParamRidgeShine, "Ridge Shine",
	              "The ridges' highlight and edge shading, and a slight residual magnification in each lens. "
	              "0 is an ideal lens.",
	              defaults.ridgeShine );
	defineSlider( desc, page, view, kParamLightAngle, "Light Angle",
	              "Where the light is, -60 to +60 degrees from the card's normal.", defaults.lightAngle );

	// The Stoatworks About block: a read-only credit line and one push button per
	// link, in a group that starts folded. Last, so it sits under the effect's
	// own controls.
	stoatworks::about::ofx::describe( desc, page );
}

OFX::ImageEffect* LenticularPluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new LenticularOFXPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static LenticularPluginFactory* factory =
		new LenticularPluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
