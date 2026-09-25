# lenticular — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 **mixer** for Resolume Arena/Avenue that shows this
layer or the layer below the way a printed lenticular card does: two pictures cut
into strips and interleaved under a sheet of cylindrical lenses, with the layer's
opacity fader tilting the card. C++17 + GLSL 4.10, CMake, universal macOS
`.bundle` (and a Windows `.dll` that has never been built). MIT. Intended home
`github.com/stoatworks-labs/lenticular`; today a local repo at `~/dev/lenticular`,
v0.1.0, unreleased, **never loaded into Resolume**. The fleet's fourth mixer,
after genlock, wipe and relay. Tranche five, started 2026-09-25; Allan picked the
idea.

`CLAUDE.md` is the command reference. This file is the *why*: the idea, every
number in the harness and where it comes from, the traps this build actually hit,
what is verified and what is assumed, and the decisions taken without asking.

For how an FFGL mixer behaves at the ABI and in Arena — the type being one
argument, the input count a separate declaration, which base class, index 0
hidden, `Opacity` bound to the layer fader and ramped by transitions — read
genlock's `AGENTS.md` and relay's "What Relay showed in Arena"
(`~/Projects/resolume/genlock/`, `~/Projects/resolume/relay/`). Nothing here
contradicts them, and none of it was re-measured here: this repo has never been
in front of Arena.

---

## The one idea

**Model the lens and the print, and the card does the rest.**

Across the picture there are N_L lenses and, under them, N_P interleave periods,
each an A strip then a B strip (both counted per picture width). A thin
cylindrical lens of focal length F (in lens pitches) focuses rays arriving at
angle t onto its focal plane at F tan t from its axis, and the print is in that
plane. So the eye looking at pixel column x — under lens k, at e from its centre —
sees the print at

    x_print = k + 1/2 + F tan t + mu e      lens pitches
    phase   = x_print N_P / N_L             print periods: fract < 1/2 is A, >= 1/2 is B

and everything a lenticular card does is a consequence:

- **The flip.** Matched pitches, viewer at infinity: the phase is k + 1/2 + F tan t,
  the same fraction under every lens, so the whole card flips at once — to B as
  tan t crosses 0, back to A as F tan t crosses 1/2, the edge of the viewing zone.
- **The ghost.** The lens focuses to a spot of width w, not a point; the eye sees
  the mean of the print across it, and near a strip edge that is both pictures.
  The print's bleed ramps the edge further. The mean is a closed form (the
  antiderivative of a square wave with linear ramps), not a sum.
- **Moiré.** Mismatched pitches: the phase advances N_P/N_L per lens instead of 1,
  so the flip happens in bands of period 1 / | 1/p_L − 1/p_P | = W / | N_L − N_P |
  pixels, and they sweep across as the card tilts, by F Δ(tan t) N_P/N_L of a band.
- **The sweep.** A viewer D picture widths away at tilt T sees column x (widths
  from the centre) at tan t = ( sin T − x / D ) / cos T, so the flip is where that
  is 0: x = D sin T.
- **The stepped, ridged look.** Every pixel of a lens shows the print at one point
  (mu = 0), so the picture is sampled once a lens; the ridges catch the light.

`Squeeze` decides what a strip holds: a real interleave squeezes each picture's
band of one period into its half-period strip (A at n + 2u), or the strip is a
window on the picture where it is (A at n + u). Where the spot centre is on the
OTHER picture's strip, each picture is taken from its nearest strip edge —
A at n + min( 2u, 1 ), B at n + max( 2u − 1, 0 ) — which is continuous in u.

The CPU does nothing but convert parameters (`Controls.cpp`) and hand them over
(`ProcessOpenGL`). There is no clock, no buffer and no state from one frame to
the next.

---

## How the mixer is wired, and what is new

The mixer mechanics are relay's, unchanged: `CFFGLPlugin` with `FF_MIXER` as the
eighth argument, `SetMinInputs( 2 )` / `SetMaxInputs( 2 )` separately, guards on
the array, the count, each pointer and a zero size (each logged once), one MaxUV
and one half-texel inset per input applied once in its own fetch, the vertex
shader passing UV through unscaled, the scoped bindings declared interleaved.

What is new:

- **Every position is computed from `floor( uv * OutSize )`.** The interpolated
  `uv` is good only to the GL's "about 1 part in 10^5" (wipe's software-renderer
  lesson, which cost wipe a red CI run). Half a pixel from any integer, a
  1e-5 error cannot change the floor below 50,000 pixels wide, so the pixel is an
  exact integer on any conforming rasteriser and everything downstream is
  arithmetic on it. This is why no check here needed the co-area allowance wipe's
  `--area` needs.
- **The phase is a whole number of periods plus a lead.** `base = ( k + 0.5 ) *
  PitchRatio`, `nb = floor( base )`, `lead = ( base − nb ) + ( F tan t + mu e ) *
  PitchRatio`. See the traps: in one product the lens index rounds into the
  fraction.
- **The coverage is a branch where the spot is on one strip's flat part.** Then
  the output is exactly one picture's texel, not a mix that happens to cancel.
- **Predictions come from what the harness asked.** `asked()` converts the
  parameters the harness set through `Controls.h`; the plugin's `SetupForTest()`
  is never an input to a prediction (see the traps).

---

## Every number in the harness

Every numeric check runs at **640×360 and 320×180** (`--mixer` at genlock's
320×200 and at 320×180), on this Mac's GPU **and** on Apple's software renderer
(`LNTEST_RENDERER=software`, what a GPU-less runner gets). Physics checks render
flat black A and flat white B into a **float** framebuffer, so a pixel IS the B
fraction m. Where a bound is computed, the check asserts that the tolerance is at
least three times it.

### The tolerance-source list

| Check | The number | Where it comes from |
|---|---|---|
| `--mixer` guards | none | Five `FFResult` comparisons. No raster, no rasteriser. |
| `--mixer` sentinel | **135** in summed channel difference | genlock's: the nearest card colour is 302 (A) and 315 (B) from the magenta padding, asserted, and half of that is unreachable by any blend of two card colours. The third render has a viewer one width away square on, so the left half of the card is B and the right A: both inputs fetched through their own MaxUV in one picture. |
| `--mixer` quadrant means | **1 of 255** | One 8-bit code. A constant region sampled anywhere inside is the constant. The inset is `ceil( out / used ) + ceil( lens px ) + 1` pixels: the width a boundary can smear over, plus the one lens a boundary can move by, because the picture is sampled at each lens's centre. |
| `--mixer` marker | **one source texel**, and **half a lens** more across | The quantum the marker's edges are drawn on; across, an edge lands on whichever lens centre it covers. A 2-pixel lens (160 across 320) keeps half a lens under a third of B's texel. |
| `--ends` | **zero bytes**, all four channels, 4 settings × 2 rasters | At the ends the spot is wholly on one strip's flat part, so coverage is a branch and the output is one fetch; the prediction asserts each lens's sample sits **at least one texel** inside a sub-block of constant colour (measured 3.33 texels squeezed, 1.67 not), so bilinear filtering returns that texel on any rasteriser. The card has three sub-blocks and four row bands a lens, each its own colour and alpha (64–255), so a wrong lens, a wrong sub-block, a wrong row or a wrong alpha is a wrong byte. Rows are sampled at their own texel centre. |
| `--flip` crossing | **1e-5 rad**; derived bound **9.1e-7** | Each m is a closed form of about eight float operations on numbers under 2 — the lens index is never in the fraction — so good to 8 ULP / w. Inside the ramp m is linear in t = F tan (a box spot over a hard edge), so the crossing from two m's a quarter spot either side is good to w × 2 × (8 ULP / w) = 16 ULP in t, whatever w is; over F ≥ 1.2 in angle; plus 2 ULP for the tilt handed over as a float sine and cosine. Measured worst 6.0e-8 rad, both renderers. |
| `--flip` whole card | **4.8e-6** spread across every pixel | Two pixels' m can differ by twice one m's bound, 2 × 8 ULP / 0.2. Bitwise equal on the GPU (and reported so); the software renderer spreads it by 6e-8 (see the traps). |
| `--moire` period | **0.02 cycles per picture width** (a fiftieth of a band across the picture); derived bound ≤ **2.9e-3** | The peak of a Hann-windowed DTFT, evaluated directly at any frequency (no bins) and found by golden section, is pulled only by the picture's OTHER spectral lines. Those are enumerated: the print's odd harmonics j, imaged by the lens cells' hold to q N_L + j ΔN and folded by the pixel grid mod W, each weighing at most (1/j²)·|sinc( j w )|/|sinc( w )| (a triangle-wave print, Bleed 1, under a box spot) times the hold's response there over the fundamental's. A line clear of the main lobe pulls the peak by its weight × the kernel's slope / the peak's curvature; one inside it (under 1.5 cycles) merges and moves it by at most a·δ/(1 − a), and must weigh under a half (measured heaviest 0.026). The kernel is the window's own closed form, checked against its direct sum to 1.6e-15. Measured 0.0002, 0.0009, 0.0005 cycles off. |
| `--moire` bands present | **amplitude ≥ 0.1** of the 0..1 swing | A floor the pitch-locked fault (no bands, amplitude ~0) cannot reach; measured 0.26–0.30. |
| `--moire` sweep | **0.03 rad** of band phase (a 200th of a band); derived bound ≤ **9.0e-3** | Same enumeration. A j = 1 line is the fundamental's own image and rotates WITH it when the card tilts, so it cannot bias a phase difference; every other line rotates j times as fast, and each of the two phases can be off by its pull, so twice the sum of weight × kernel. The worst case is the whole-number one at 320 wide, where the lens sampling folds the 7th and 9th harmonics exactly onto the fundamental. Measured 3.2e-3, 5.7e-4, 6.8e-4 rad off. |
| `--distance` | **0.05 px**; derived bound ≤ **0.0035 px** | The flip's position is the sum of m across a window holding the one ramp (a linear functional, so a translated ramp moves the sum exactly, genlock's lesson). The sum of a linear ramp at pixel centres is its integral but for its two kinks, each at most slope / 8 px²; plus 8 ULP / w of m on each ramp pixel; plus a few ULP of tan carried to x through D / cos T. The window is derived from the geometry (half way between this ramp and the next strip edge's) and asserted to hold exactly one ramp and to fit the picture. Measured worst 0.0009 px. |
| `--opacity` | **1e-5 rad**; derived bound **6.0e-7** | m to 8 ULP / w, the tilt from it to w × that / F, plus 2 ULP of the tilt's sine and cosine. The inversion m → t is the box spot's linear ramp, valid because F tan( max ) = 0.2 is inside the ramp's ±0.225. Measured worst 5.5e-8 (GPU), 4.0e-8 (software). |
| `--mutation` | **fails** | See below. |
| `--bench` | not asserted | No threshold is worth asserting on somebody else's GPU, on a machine shared with seven other builds. |

**Negative controls live in the shipping class.** `Lenticular::SetFaultForTest`
takes a bitmask of `lenticular::Fault`; the shipped plugin carries 0 and nothing
but the harness sets it. Each builds the wrong answer into the real
`ProcessOpenGL` or the real shaders:

| Fault | Check | What it does, and what fails |
|---|---|---|
| `kFaultFoldedMaxUV` | `--mixer` | The vertex shader folds A's MaxUV into the varying and the fragment shader treats it as texture space for both inputs — the genlock trap, done the way the trap does it. Fails 4 assertions at 320×200, 5 at 320×180. |
| `kFaultSinFocal` | `--flip` | The focus at F sin t instead of F tan t. Fails the two zone-edge crossings at F 1.2 (atan vs asin: 22.6° against 24.6°) and, correctly, not the square-on one. |
| `kFaultFlatDistance` | `--distance` | The viewer at infinity whatever Distance says. Fails both tilts tried. |
| `kFaultPitchLocked` | `--moire` | The lens pitch forced to the print's, the spec's negative control. No bands: fails the amplitude floor and the period at a 6.5-band mismatch. |
| `kFaultTiltReversed` | `--opacity`, `--moire` | Opacity tilts the card the other way: not monotone rising, and the bands sweep the wrong way. |

Plus picture-level negatives with no fault: `--ends` rejects the other Squeeze's
samples, the card at the flip and the card with shine; `--flip` rejects a card at
Distance 2 as one value.

### Would this hold on another rasteriser, at another raster?

- `--mixer`: yes — constant regions, one source texel and one lens, as genlock
  argued; the lens count scales with the raster (2 px a lens at both).
- `--ends`: yes — a branch and a fetch at least a texel inside a constant
  sub-block; the only rasteriser dependence is that GL_LINEAR of a constant region
  is the constant. Lenses are 20 whole pixels at both rasters, so a pixel's lens is
  never within half a pixel of ambiguous.
- `--flip`: yes — no raster in it: at infinity every pixel computes from the same
  uniforms, and the crossing is arithmetic on m. The whole-card spread is asserted
  to a float bound, not bitwise, because the software renderer is not bitwise.
- `--moire`: yes — both rasters give lenses of whole pixels (10 and 5), the
  leakage bound is computed for each raster's own W, and the positions come from
  the integer pixel, not from `uv`.
- `--distance`: yes — a linear functional over a window derived from this raster;
  the kink residual scales as 1 / (ramp width in pixels) and is computed for it
  (0.0023 px at 640, 0.0035 at 320).
- `--opacity`: yes — no raster in it, as `--flip`.
- `--mutation`: the default path and the hook are two compiles of the same text
  in one context; a compiler that compiled the same source two ways would fail it.
- Two rasterisers have run all of it: this Mac's GPU and Apple's software
  renderer, at both rasters, with the same numbers to the printed digit except the
  whole-card spread and `--opacity`'s last digit. llvmpipe and any other GPU have
  not.

### The mutation

**Shipped (`--mutation`, run by `verify.sh`):** `float base = ( k + 0.5 ) *
PitchRatio;` → `( k + 0.6 )`, one character: the strip edge moves a tenth of a
period off each lens's axis. It fails **3 of 3** `--flip` crossings (each 0.1 / F
off in tan); the shipped text through the same hook passes and renders the
default path's bytes.

**By hand, once, on the working tree (2026-09-25):** in `source/Shaders.cpp`,
`float tanView = ( SinTilt - across ) / CosTilt;` → `( SinTilt + across )` — one
character, the viewer's offset across the card added instead of subtracted. The
dev build then failed **17 `--distance` assertions** (the flip landing 3.1 to
92.6 px from W (½ + D sin T)) and nothing else: `--mixer`, `--ends`, `--flip`,
`--moire`, `--opacity` and `--mutation` passed, correctly, because every one of
them has the viewer at infinity (or, in `--mixer`'s third frame, only asks that
both inputs appear). Reverted from a copy, `touch`ed against the same-second make
trap, and `--distance` passed again.

---

## The traps

Ordered by how much time they cost. Only the ones this build actually hit; where
one was caught in design rather than in a run, it says so.

**The leakage bound's first version flagged a full-weight line 6e-5 cycles from
the fundamental — the pixel grid's alias of the fundamental itself.** At 640 wide
with 64 lenses, q = 10 lens images is 640 = W, so the hold's image of the
fundamental folds back onto it, 6e-5 away because N_L is 63.99999 after the
slider's float. The weight was 1 because the bound had left out the lens cell's
hold response, |sinc( q + j ΔN / N_L )|, which is about 0.012 there. With the
hold in, and lines inside the main lobe bounded by the two-lobe merge a·δ/(1 − a)
instead of the slope formula (which is only valid clear of the lobe), the
frequency bound fell to ≤ 2.9e-3. The measurements had been right all along
(0.0002–0.0009 cycles off); the bound was wrong.

**The phase bound counted what cannot bias it and missed what can.** The fundamental's own images
(j = 1) rotate with it when the card tilts, so they cancel out of a phase
difference; they had been counted. A phase difference is two measurements, each
with its own error, so the sum needs a factor of 2; it had been missing. And in
the whole-number case (8 bands at 64 lenses) the 7th and 9th harmonics fold
EXACTLY onto the fundamental's frequency — harmless to the peak, not to the phase,
because they turn 7 and 9 times as fast. Fixed, the whole-number case's bound is
9.0e-3 rad, which is why the sweep's tolerance is 0.03 and not the 0.02 first
written: 0.02 was under three times the derived bound, and the measured 3.2e-3 is
inside the bound, as it should be.

**A prediction read from the plugin follows a fault instead of catching it.** The
band-sweep prediction used the tilt from `SetupForTest()`, the plugin's report of
what it used. With `kFaultTiltReversed` on, the prediction reversed with the
picture and the check passed. Every prediction now comes from `asked()` — the
parameters the harness set, through `Controls.h` — and the reversed tilt fails
the sweep. `--ends` and `--distance` had the same weakness and were moved to
`asked()` before any fault exercised it.

**Apple's software renderer is not bitwise across pixels that compute from the
same uniforms.** At infinite distance with matched pitches every pixel's m is a
function of uniforms only, and on this Mac's GPU every pixel is the same float,
bit for bit. On the software renderer they spread by one ULP (6e-8) — the
"whole card at once" check failed there on its first run with every tolerance
passing. Why is not known (per-lane code paths are a guess, not a finding). The
claim was never one GLSL makes, so it is now a float bound (4.8e-6) and the output
says "bitwise" or "not bitwise" beside it.

**In one product, the lens index rounds into the phase.** `( k + 0.5 + F tan t ) *
PitchRatio` at k ≈ 60 has an ULP of 3.8e-6 periods, different for every lens, so
the whole card is not one number and a check of it is about rounding.
Caught in design, working the float budget for `--flip` before its first run: the
phase is now a whole number of periods plus a lead under two.

**`git submodule add --reference` leaves the submodule borrowing another repo's
objects.** It writes an `alternates` file into
`.git/modules/external/ffgl/objects/info/` pointing at tinsel's modules
directory under `~/Projects`, so this repo's SDK checkout would break the day
tinsel's did. Dissociated at creation: `git -C external/ffgl repack -a -d`, then
the alternates file removed, then `fsck`.

**A nested heredoc ends the outer one.** Writing verify.sh's pipe block from a
`python3 - <<'PY'` script that itself contained a `<<'PY'` heredoc terminated the
outer one at the inner `PY` and handed zsh the rest. Tooling, not the plugin;
the adaptation scripts now live in files.

**Bleed and Focus Spot are dead at the default Opacity, correctly.** At Opacity
1 the default spot (0.1 lens) sits wholly on B's flat part, so neither changes a
pixel. Anticipated in design; the sweep sweeps them at Opacity 0.5, the flip, and
says why in its docstring.

**The worktree guard judges by the session's directory, not by a `cd`.** The
first commit, written `cd ~/dev/lenticular && git commit`, was BLOCKED as "a
shared checkout" of `stoatworks-backend` — the directory this session started
in, under `~/Projects`. `git -C /Users/allansargeant/dev/lenticular commit`
passes, as the machine's rule (always `git -C`) says it should. No
`CLAUDE_WORKTREE_EXEMPT` was needed or used anywhere in this repo.

Inherited and avoided on cue: the scoped bindings clearing rather than restoring;
`StoatworksAboutParams.h` needing the SDK first; the OBJECT library; the 0..1
clamp on STANDARD defaults; the `SetTextParameter` override; GLSL reserved words
(`packed`, `smooth`, `round`, `half`, … none used); MSVC's missing `M_PI` (`kPi`),
`<cmath>` included, no `near`/`far`.

---

## Shape of the code

    source/Shaders.cpp      the pass. One vertex, one fragment: the lens, the
                            print's phase, the spot's coverage in closed form,
                            the two fetches, the ridge.
    source/Lens.h           the geometry, written down; the model constants
                            (ridge slope, highlight, shading, residual
                            magnification); the per-frame Setup.
    source/Lenticular.*     the plugin: type, parameters, the two inputs, the
                            parameters turned into uniforms.
    source/Controls.*       0..1 host parameters to counts, lens pitches,
                            picture widths, radians.
    source/Diag.*           a log file: the host, the inputs' sizes, what drove
                            Opacity, each guard once.
    tools/lntest/           the offline harness. Two inputs, a float FBO.
    tools/sweep.py          no control is silently dead.
    tools/verify.sh         all of it.

---

## Decisions taken without asking

**Opacity 0 is A, 1 is B, 0.5 is square on.** The spec maps the tilt from −max to
+max; which end is which is a choice, and this one makes the layer fader behave
like every other mixer's: down is the layer below, up is this layer. `Opacity`
defaults to 1, as relay's does — a mixer dropped on a layer at full opacity shows
this layer, and in Resolume the fader overrides it at once.

**The strip edge is registered to the lens axis.** The phase at a lens's centre is
exactly half a period, so square on is the flip. A card whose strips were printed
off-axis would flip at another angle; that registration offset is not a control.

**Both pitches are counts per picture width on one geometric scale, 4 to 480.**
The spec's "strips per unit width": so a setting means the same thing at 720p and
4K, and the same slider position is the same pitch bit for bit, which is the only
way an operator can MATCH them. The cost: a detune is fine-grained on a geometric
slider — one thousandth of the slider is a 0.48% pitch change, 0.29 bands a width
at 60 lenses. Default 60, matched.

**The lens is a thin lens in air: the focus at F tan t.** A real sheet refracts
into the plastic first (sin t = n sin t'), which would put the zone edge where
Snell's law puts it rather than at atan( 1 / 2F ). F is therefore an effective
focal length in air, not the sheet's thickness. `--flip` measures the thin-lens
law, and its negative control (F sin t) is rejected.

**The focus spot is a box, and constant across the tilt.** Real aberration grows
off-axis and is not flat-topped. A box keeps the coverage a closed form and makes
the flip's ramp linear in tan t, which is what `--flip` and `--opacity` read the
tilt off; a Gaussian would need its own inversion and would not change what the
checks prove.

**Focus Spot is in lens pitches, Bleed in strips.** The spot is the lens's
property, the bleed the print's. The spot is clamped to 4 print periods (a lens far
coarser than the print would otherwise average dozens of periods through a closed
form that is only as good as its float cancellation).

**Distance is linear in 1/D, and infinity is a value.** The thing Distance does to
the picture, the spread of angle across the card, is linear in 1/D; slider 1.0 is
exactly 1/D = 0, the flat flip, not a limit approached. Default 5 widths: the flip
sweeps across the middle of the fader travel, and both ends are still clean
(−8° ± 5.7° stays inside the 15.5° zone at the default F 1.8).

**Angle Range defaults to 8°.** At F 1.8, F tan 8° = 0.253: the ends land at the
middle of each strip, as far from a flip as the lens allows.

**Squeeze is index 0, on.** Arena hides a mixer's first parameter (three for
three). A real interleave squeezes; off is a curiosity, not a working setting, so
it is the one control whose default is right for ever.

**The "slight lens distortion" is coupled to Ridge Shine.** The spec's control list
has no slot for it. At Ridge Shine s each lens shows 0.12 s of a lens width of
print across its width — the residual magnification a slightly defocused lens
shows — together with the highlight and the edge shading. At Ridge Shine 0 the
lens is ideal, which is what every physics check runs at.

**Output alpha is the seen picture's.** The mix of A's and B's alpha by the B
fraction, so at the ends it is the sampled input's alpha untouched (`--ends`
asserts all four channels on cards whose alpha runs 64–255; Resolume's DXV demo
clips carry alpha). The ridge highlight is composited OVER as white of coverage
h, so a transparent picture behind a shiny sheet still shows the shine, as the
plastic would. Premultiplied alpha is assumed, as the fleet assumes it.

**No end stops.** Opacity 0 and 1 are not a bitwise A and B, as wipe's are: they
are A and B *through the lens*, stepped and ridged. That is the product, but it
means a transition that ends on this mixer will pop from the card to the plain
clip when Resolume stops using it. See the open questions.

**No clock.** Nothing in a card moves unless the viewer or the card does, so
`SetTime` is not overridden and the harness drives no clock. The fleet's float
clock trap does not arise.

**No presets.** Eleven controls in three groups.

**The About block is a provisional hand copy**, with `guide = ""`: no user guide
exists, and the links header leaves a missing link out rather than showing a
button that opens a 404. Three buttons: Project page, Source on GitHub, Support
the work.

---

## What is genuinely verified, and what is assumed

**Verified, by measurement, on this machine (Apple M4 Max, macOS 26.4.1),
2026-09-25, at 640×360 and 320×180, on the GPU and on Apple's software
renderer:** everything in the tolerance table, and the README's Status table
restates it with the numbers. `tools/verify.sh` runs all of it on a fresh
universal build: 2 shaders compile through glslc, 8 suites on the GPU and 7 on the
software renderer, 11 controls swept, the pipe's three assertions, `plugMain`
exported, `x86_64 arm64`, plist, ad-hoc codesign, `oxbow probe` reading SW
Lenticular / LN01 / mixer / inputs 2..2 / Squeeze (boolean, on) at 0 / a standard
Opacity.

**The render cost**, `lntest --bench`, 120 frames after a 20-frame warm-up,
`glFinish` both sides, worst of three runs (defaults at the flip; the moiré case
is the same pass and costs the same):

| | ms/frame | % of a 60fps frame |
| --- | --- | --- |
| 1280×720 | 0.021 | 0.1% |
| 1920×1080 | 0.035 | 0.2% |
| 2560×1440 | 0.055 | 0.3% |
| 3840×2160 | 0.104 | 0.6% |

One pass and two texture fetches. As genlock found, a tenth of a millisecond is
close to what a `glFinish` round trip costs to observe, and 4K came back at 0.038,
0.082 and 0.104 on three runs on a shared machine: take the ceiling.

**Assumed, or not yet done:**

- **Never loaded into Resolume**, on macOS or Windows. Everything Arena does with
  a mixer — Extra Effects, Blend Mode and transition lists, index 0 hidden,
  `Opacity` bound to the layer fader and ramped by a transition, both inputs
  padded — is genlock's, wipe's and relay's measurement, not this plugin's.
- **No CI, no Windows build.** The workflows are relay's, renamed; they have never
  run. The MSVC hazards the fleet knows (`M_PI`, `<cmath>`, `near`/`far`) were
  checked by reading, not by compiling.
- **Two rasterisers, not all.** Nothing has run on llvmpipe (the Arena gate's
  renderer, which found `packed` for atrac) or another GPU. glslc compiles both
  shaders; Mesa has not seen them.
- **The optics are a model.** A thin lens in air, a box spot, a trapezoid print,
  a viewer's eye as a point. The ridge highlight (a 35° lens edge, a 0.06-lens
  highlight), the edge shading and the residual magnification are looks, argued
  in `Lens.h`, measured against nothing.
- **Premultiplied alpha** is assumed, as the fleet assumes it.
- **Vertical lenticules only.** Real cards are also made with horizontal ones
  (flip by tipping, not turning); not a control.
- **The hero image** is the harness's render, not Resolume's.
- No user guide, no presets, no OpenFX port, no browser demo.

---

## Open questions

1. **Should Opacity 0 and 1 be end stops?** A transition that uses this mixer
   ends with a pop from the lenticular card to the plain clip. An option that
   crossfades the lens away over the last few percent of the fader would fix it
   and break `--ends`' meaning at that setting. Not attempted: it needs Arena to
   show what a transition's last frame looks like.
2. **Should the print's registration to the lens be a control?** A card printed
   a fraction of a strip off-axis flips at another angle — a real defect, one
   parameter away (it is the constant `0.5` the shipped mutation moves).
3. **Should Interleave Pitch be a detune instead?** An absolute count on a
   geometric slider makes matching easy and small detunes coarse-ish (0.48% a
   thousandth of the slider). A "Misregistration" control in parts per thousand
   of the lens pitch would make moiré finely playable, at the cost of the spec's
   "strips per unit width".
4. **Refraction.** Modelling the plastic's index moves the zone edge from
   atan( 1 / 2F ) to where Snell puts it; a Refractive Index control, or a fixed
   1.5 with F meaning the real thickness, is the physically honest version.
5. **More than two frames.** The spec's "N-frame interleave with in-between
   frames as blends" was left at N = 2: two inputs, two strips, and the ghost is
   the in-between. An N-strip print of blends of A and B is a Frames control away,
   and the coverage integral generalises (N ramps a period).
6. **Does Arena hide Squeeze, and does a transition drive the tilt?** Three for
   three on the previous mixers; not measured on this one. The diag log records the
   first 16 Opacity changes for exactly this.

---

## Notes

Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes). What this build
adds to the mixer account: position from `floor( uv * size )` so no check needs
the interpolator's allowance; a phase taken relative to its own period; and
predictions from the parameters asked, never from the plugin's report.
