#include "Controls.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lenticular
{
namespace
{
double clamp01( double value )
{
	return std::min( 1.0, std::max( 0.0, value ) );
}

/// lo * ( hi / lo )^t: geometric between two ends.
double geometric( double t, double lo, double hi )
{
	return lo * std::pow( hi / lo, clamp01( t ) );
}

double geometricParam( double value, double lo, double hi )
{
	if( !( value > 0.0 ) )
		return 0.0;
	return clamp01( std::log( value / lo ) / std::log( hi / lo ) );
}

constexpr double kDegree = kPi / 180.0;
} // namespace

double PitchFromParam( float value )
{
	return geometric( value, kPitchMin, kPitchMax );
}

double FocalFromParam( float value )
{
	return geometric( value, kFocalMin, kFocalMax );
}

double SpotFromParam( float value )
{
	return clamp01( value ) * kSpotMax;
}

double BleedFromParam( float value )
{
	return clamp01( value ) * kBleedMax;
}

double InvDistanceFromParam( float value )
{
	//1.0f gives exactly 0: infinity is a value the slider can hold, not a
	//limit it approaches.
	return ( 1.0 - clamp01( value ) ) * kInvDistanceMax;
}

double AngleRangeFromParam( float value )
{
	return clamp01( value ) * kAngleRangeMaxDeg * kDegree;
}

double TiltFromParams( float opacity, float angleRange )
{
	//2 o - 1 is exact in float for every o the host can send, and exactly 0
	//at o = 0.5: the fader's middle is the card seen square on.
	return ( 2.0 * clamp01( opacity ) - 1.0 ) * AngleRangeFromParam( angleRange );
}

double LightFromParam( float value )
{
	return ( 2.0 * clamp01( value ) - 1.0 ) * kLightMaxDeg * kDegree;
}

float ParamForPitch( double periodsPerWidth )
{
	return static_cast< float >( geometricParam( periodsPerWidth, kPitchMin, kPitchMax ) );
}

float ParamForFocal( double lensPitches )
{
	return static_cast< float >( geometricParam( lensPitches, kFocalMin, kFocalMax ) );
}

float ParamForSpot( double lensPitches )
{
	return static_cast< float >( clamp01( lensPitches / kSpotMax ) );
}

float ParamForBleed( double fractionOfStrip )
{
	return static_cast< float >( clamp01( fractionOfStrip / kBleedMax ) );
}

float ParamForDistance( double pictureWidths )
{
	if( !( pictureWidths > 0.0 ) || std::isinf( pictureWidths ) )
		return 1.0f;
	return static_cast< float >( clamp01( 1.0 - ( 1.0 / pictureWidths ) / kInvDistanceMax ) );
}

float ParamForAngleRange( double radians )
{
	return static_cast< float >( clamp01( radians / ( kAngleRangeMaxDeg * kDegree ) ) );
}

float ParamForOpacity( double tilt, double range )
{
	if( !( range > 0.0 ) )
		return 0.5f;
	return static_cast< float >( clamp01( 0.5 + 0.5 * tilt / range ) );
}

float ParamForLight( double radians )
{
	return static_cast< float >( clamp01( 0.5 + 0.5 * radians / ( kLightMaxDeg * kDegree ) ) );
}

} // namespace lenticular
