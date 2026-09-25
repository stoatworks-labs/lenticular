#pragma once

/**
    The card, as numbers: what one frame's parameters make of the lens sheet
    and the print under it. No GL.

    ------------------------------------------------------------ the geometry

    Across the picture there are N_L lenses and, under them, N_P interleave
    periods, each an A strip then a B strip (both counts per picture width).
    Everything the shader does is in LENS units: pixel column x, at picture
    fraction X = ( x + 0.5 ) / W, sits under lens k = floor( X N_L ), at
    e = fract( X N_L ) - 0.5 from its centre.

    A thin cylindrical lens of focal length F (in lens pitches) focuses rays
    arriving at angle t from the normal onto its focal plane at F tan t from
    its axis -- the chief ray goes through the centre undeviated. The print
    is in that plane, so the eye looking at lens k sees the print at

        x_print = k + 0.5 + F tan t + mu e          (lens pitches)

    and its phase in the interleave is phi = x_print N_P / N_L periods. Strip
    A is the first half of each period (fract phi in [0, 0.5)), B the second.
    mu is the residual magnification of a lens not perfectly focused -- the
    "slight lens distortion" -- and is 0 unless Ridge Shine is up.

    - **Matched pitches, viewer at infinity**: phi = k + 0.5 + F tan t, so
      fract( phi ) is the same under every lens and the whole card flips at
      once: to B as tan t crosses 0, back to A as F tan t crosses +1/2 (the
      edge of the viewing zone; tilt further and the next lens's strips come
      round), and to A below 0 down to -1/2.
    - **Mismatched pitches**: phi advances by N_P / N_L per lens instead of
      1, so fract( phi ) drifts across the card and the flip happens in bands
      of period 1 / | 1/p_L - 1/p_P | = W / | N_L - N_P | pixels, which sweep
      across as the card tilts. That is the moire of misregistration.
    - **A viewer at distance D** (picture widths) at tilt T from the card's
      centre sees each column at its own angle: tan t = ( sin T - x / D ) /
      cos T, x in widths from the centre. The flip is where tan t = 0, at
      x = D sin T, so it sweeps across the card as the card tilts.

    Squeeze decides what a strip holds. A real interleave resamples each
    picture to one column in N and cuts those into the strips (Squeeze on):
    the A strip of period n holds A's band [ n, n + 1 ) periods squeezed into
    half a period, so the phase fract( phi ) = u maps to A at n + 2u. Without
    Squeeze the strips are windows on the picture at its own position, so the
    print at phi holds A (or B) at phi. Where the spot centre is on the
    OTHER picture's strip (the ghost), each picture is taken from its nearest
    strip edge, which is continuous in u: A at n + min( 2u, 1 ), B at
    n + max( 2u - 1, 0 ).

    The focus spot is a box of width w on the print; the eye sees the mean of
    the print across it, so the B fraction m is the mean of the strip pattern
    over the box -- in closed form (Shaders.cpp) from the antiderivative of a
    square wave with linear ramps of width beta between strips (the bleed).
*/

namespace lenticular::lens
{

/// The lens edge's slope, which sets how far across a lens the ridge's
/// highlight can travel: at 35 degrees, a light and a viewer 70 degrees
/// apart (between them) still put it on the lens. A look, not a
/// measurement of any sheet.
inline constexpr double kRidgeEdgeSlopeDeg = 35.0;

/// The highlight's half-width across the lens, in lens pitches, and its
/// floor in output pixels so it never aliases to nothing between columns.
inline constexpr double kHighlightWidth   = 0.06;
inline constexpr double kHighlightMinPx   = 0.75;

/// How much darker the lens is at its edges than its centre at Ridge Shine
/// 1 (the curved surface catching less of the room). A look.
inline constexpr double kEdgeShade = 0.25;

/// The residual magnification at Ridge Shine 1: how much of the print one
/// lens shows across its width when it is not perfectly focused. A look,
/// coupled to Ridge Shine because the spec's control list has no slot of
/// its own for it; at Ridge Shine 0 the lens is ideal.
inline constexpr double kDistortMax = 0.12;

/// The spot is clamped to this many print periods: a lens pitch much
/// coarser than the print would otherwise average dozens of periods to grey
/// through a closed form that is only as good as its float cancellation.
inline constexpr double kSpotPeriodsMax = 4.0;

/// Everything one frame's parameters make of the card, in the units the
/// shader takes.
struct Setup
{
	bool squeeze        = true;
	double lensPerWidth = 60.0;///< N_L
	double printPerWidth = 60.0;///< N_P
	double ratio        = 1.0;///< N_P / N_L: print periods per lens
	double focal        = 1.8;///< F, lens pitches
	double spotLens     = 0.0;///< w, lens pitches
	double spotPeriods  = 0.0;///< w N_P / N_L, print periods, clamped
	double bleedPeriods = 0.0;///< beta: the ramp between strips, print periods
	double invDistance  = 0.0;///< 1 / D, per picture width; 0 is infinity
	double tilt         = 0.0;///< T, radians
	double shine        = 0.0;
	double light        = 0.0;///< radians
	double distort      = 0.0;///< mu
};

} // namespace lenticular::lens
