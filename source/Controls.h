#pragma once

/**
    Host parameters are 0..1; these are what they mean.

    Every ranged parameter this plugin declares is a plain FF_TYPE_STANDARD
    float in 0..1, including the ones that stand for a count of lenses, a
    focal length in lens pitches, a distance in picture widths or an angle in
    degrees. That is not a style preference: `SetParamInfo` clamps a standard
    default into 0..1 *before* returning, and `SetParamRange` can only be
    called afterwards -- so a parameter declared in degrees cannot declare a
    default in degrees. The conversions live here, in one file the plugin and
    the harness both use, so there is only ever one answer to what a slider
    position means. Squeeze is FF_TYPE_BOOLEAN.

    **Both pitches are counts across the picture WIDTH**, not pixels: a card
    with 60 lenses has 60 at 720p and at 4K, so a setting means the same
    thing twice. They share one mapping, so the same slider position is the
    same pitch, bit for bit -- which is what "matched" means to an operator.
*/
namespace lenticular
{

/// Lens Pitch and Interleave Pitch: periods across the picture width,
/// geometric. One lens period holds one A strip and one B strip, and one
/// interleave period is one A strip and one B strip of the print. 4 is a
/// card of fat ridges; 480 at 1080p is four pixels a lens, where the
/// sampling itself starts to alias.
inline constexpr double kPitchMin     = 4.0;
inline constexpr double kPitchMax     = 480.0;
inline constexpr double kPitchDefault = 60.0;

/// Focal Length: in lens pitches, geometric. The print sits in the lens's
/// focal plane. It is an effective focal length in air -- a thin lens, no
/// refraction into the plastic (see AGENTS.md). A viewing zone -- the
/// tilt across which one strip is seen -- is atan( 1 / ( 2 F ) ) either side
/// of the flip: 45 degrees at F 0.5, 15.5 at 1.8, 4.8 at 6. Real sheets are
/// around 1.5 to 2.5.
inline constexpr double kFocalMin     = 0.5;
inline constexpr double kFocalMax     = 6.0;
inline constexpr double kFocalDefault = 1.8;

/// Focus Spot: the width of the lens's focus on the print, in lens pitches,
/// 0 to 0.5, linear. A box: the eye sees the mean of the print across it.
/// This is the aberration that makes the ghost.
inline constexpr double kSpotMax = 0.5;

/// Bleed: the width of the ramp between two strips, as a fraction of one
/// strip, 0 to 1, linear. At 1 the print is a triangle wave.
inline constexpr double kBleedMax = 1.0;

/// Distance: the viewer's distance from the card, in picture widths, as its
/// INVERSE -- linear in 1 / D from 2 (half a picture width away) at 0 to 0
/// (infinity: the whole card flips at once) at 1. Linear in 1 / D because the
/// thing it does to the picture, the spread of viewing angle across the
/// card, is linear in 1 / D.
inline constexpr double kInvDistanceMax = 2.0;

/// Angle Range: the tilt Opacity 0 and 1 stand for, 0 to 45 degrees.
inline constexpr double kAngleRangeMaxDeg = 45.0;
inline constexpr double kAngleRangeDefaultDeg = 8.0;

/// Light Angle: -60 to +60 degrees from the card's normal.
inline constexpr double kLightMaxDeg = 60.0;
inline constexpr double kLightDefaultDeg = 25.0;

double PitchFromParam( float value );          ///< periods per picture width
double FocalFromParam( float value );          ///< lens pitches
double SpotFromParam( float value );           ///< lens pitches
double BleedFromParam( float value );          ///< fraction of a strip
double InvDistanceFromParam( float value );    ///< per picture width; 0 is infinity
double AngleRangeFromParam( float value );     ///< radians
double TiltFromParams( float opacity, float angleRange );///< radians, -range .. +range
double LightFromParam( float value );          ///< radians

float ParamForPitch( double periodsPerWidth );
float ParamForFocal( double lensPitches );
float ParamForSpot( double lensPitches );
float ParamForBleed( double fractionOfStrip );
/// 0 or infinity gives exactly 1.0f.
float ParamForDistance( double pictureWidths );
float ParamForAngleRange( double radians );
float ParamForOpacity( double tilt, double range );
float ParamForLight( double radians );

inline constexpr double kPi = 3.14159265358979323846;

} // namespace lenticular
