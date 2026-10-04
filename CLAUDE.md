# lenticular

A printed lenticular sheet that shows one layer or the other, as an FFGL
**mixer** for Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal
`.bundle` (macOS) + Windows x64 `.dll` — and the same card as an **OpenFX
Transition** (`Lenticular.ofx.bundle`, macOS universal / Win64 / Linux x86-64)
for Resolve and Vegas, rendered on the CPU. MIT. Public at
github.com/stoatworks-labs/lenticular, released v0.2.0 (which added the OpenFX
build); loaded in Resolume
Arena 7.27.1 on Windows (AGENTS.md, "What Lenticular showed in Arena"), never on
macOS. The fleet's fourth mixer, after genlock, wipe and relay.

Read `AGENTS.md` before changing the geometry, the coverage integral or any
tolerance in the harness. Read `~/Projects/resolume/genlock/AGENTS.md` (the
fleet's account of how an FFGL mixer behaves) and
`~/Projects/resolume/relay/AGENTS.md` ("What Relay showed in Arena") before
touching `ProcessOpenGL`.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install into Arena: `cmake --install build` → `~/Documents/Resolume Arena/Extra Effects`
  (yes, Extra Effects, although this is a mixer: Arena has one FFGL folder and
  no `Extra Mixers` — measured on genlock and wipe.)
- Render a frame offline: `./build/lntest --out /tmp/f.png --size 1920x1080`
  (add `--twin` to render it through the OpenFX build's CPU twin instead)
- The OpenFX bundle builds alongside: `build/Lenticular.ofx.bundle`
  (`-DBUILD_OFX=OFF` skips it; `-DLENTICULAR_BUILD_FFGL=OFF` builds it alone
  with nothing but a compiler, as the Linux release job does). Not installed
  by `cmake --install`: it goes in `/Library/OFX/Plugins` by hand.
- Choose the two inputs: `--input-a video --input-b graphic`
  (a is **A**, the layer below, printed in each lens's first strip; b is **B**,
  this layer, in the second. Also `quads-a`, `quads-b`, `bars`, `black`,
  `white`, `flat`.)
- Set anything by name, in host units (0..1): `--set "Opacity=0.5" --set "Interleave Pitch=0.58"`
- List parameters: `./build/lntest --list`
- Film through it (the fleet's `--pipe` format, with a second input):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/lntest --pipe --size WxH
  [--pipe-src FILE_OR_FIFO] [--src-size WxH] [--script cues.txt] |
  ffmpeg -f rawvideo -pix_fmt rgba -s WxH -i - out.mov`. stdin is A (the layer
  below), `--pipe-src` is B (this layer; without it `--input-b` is held).
  Cues are `frame Name value`, value in host units. **Standard parameters ramp
  between keys; booleans (and any option or event) step.** The first key holds
  before it. Unknown names and the About block are refused. Lenticular has no
  clock, so only cues move the picture. A partial frame at EOF ends the run.
  SIGPIPE is ignored: a closed stdout is exit 1.

## Verify
- Everything: `tools/verify.sh` (fresh universal build + every check, ~20 s here)
- No name over 16 characters or duplicated, parameter 0 is Squeeze (boolean,
  on), the tilt is Opacity, the pitches default matched: `./build/lntest --names`
- Two inputs, two sizes, two MaxUVs, and the guards: `./build/lntest --mixer`
- Tilted all the way, A or B sampled at the lens pitch, bitwise, alpha
  included, Squeeze on and off: `./build/lntest --ends`
- The whole card flips square on and back at ±atan( 1 / 2F ): `./build/lntest --flip`
- A pitch mismatch bands at 1/|1/p_L − 1/p_P|, by FFT, and the bands sweep by
  the predicted phase as the card tilts: `./build/lntest --moire`
- A near viewer's flip lands at x = D sin T: `./build/lntest --distance`
- Opacity tilts the card monotonically, −max to +max: `./build/lntest --opacity`
- One character of the shipped GLSL fails --flip: `./build/lntest --mutation`
- The OpenFX build's CPU twin of the shader against the GPU, per pixel, 14
  settings, with three negative controls: `./build/lntest --cpu`
- The OpenFX build's ends — exactly the clips at 0 and 1, a smooth ramp, the
  plain picture bit for bit — with no GL: `./build/lntest --fade`
- The OpenFX bundle loads and describes as a Transition:
  `../resolume-ofx-bridge/build/ofxprobe --dir build`. The bridge's origin/main
  probe cannot render a transition (Filter context only). A probe that can
  (`--context transition`, e.g. the 2026-10-03 test host) renders it:
  `OFXPROBE=/path/to/that/ofxprobe tools/verify.sh` then compares five
  Ends = Cut settings with the GPU (`lntest --pipe`), rejects a control, and
  checks Fade's ends byte for byte against the clips and two mid-ramp points
  against the crossfade.
- ms/frame, 720p through 4K: `./build/lntest --bench` (never loop it at 4K on
  this shared machine)
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Any check on Apple's software renderer, as the GPU-less CI runner gets it:
  `LNTEST_RENDERER=software ./build/lntest --moire` (verify.sh runs them all)
- The browser demo's shaders are still the plugin's:
  `python3 demo/tools/check_shaders.py` (verify.sh runs it)
- The harness writes the plugin's log; point `LENTICULAR_LOG_DIR` elsewhere
  before running it around an Arena session (verify.sh does).

Every numeric check runs at 640x360 and 320x180 (`--mixer` at 320x200 and
320x180) and carries its own negative control.

## Notes
- **This is an `FF_MIXER`.** The type is the eighth argument of
  `CFFGLPluginInfo`; `SetMinInputs`/`SetMaxInputs` are a **separate**
  declaration. `verify.sh` asserts both through `oxbow probe`.
- **`inputTextures[0]` is A (the layer below), `[1]` is B (this layer).** Each
  has its own `MaxUV` and half-texel inset, applied once in its own fetch. The
  vertex shader passes UV through unscaled — never fold MaxUV into it (the
  genlock trap; `kFaultFoldedMaxUV` is exactly that, and `--mixer` fails it).
- **The scoped bindings are declared interleaved** — activate(0), bind(0),
  activate(1), bind(1) — because each clears to 0 on exit rather than restoring.
- **Every position comes from `floor( uv * OutSize )`**, an integer pixel,
  never from the interpolated `uv` itself, which the GL promises only to 1 part
  in 10^5 (wipe's CI lesson). Exact on any conforming rasteriser under 50,000 px.
- **The phase is a whole number of periods plus a lead** (`base`/`nb`/`lead` in
  the shader), so the lens index never rounds into the fraction: with matched
  pitches the whole card is one number. Do not fold it back into one product.
- **The coverage is a branch on the flat parts** of the strips: a focus spot
  wholly on one strip returns exactly 0 or 1, and the fetch of that picture
  only, which is what makes `--ends` a bitwise claim. Test B's flat region
  before A's: at a zero spot on the edge u = 0.5 exactly, B is right.
- **Opacity is the tilt, and the name is load-bearing.** Resolume binds a
  mixer parameter named `Opacity` to the layer's opacity fader and a transition
  ramps it (measured on genlock, wipe and relay). 0 is −Angle Range (A), 1 is
  +Angle Range (B), 0.5 exactly square on.
- **Parameter 0 is sacrificial.** Resolume Arena does not expose a mixer's first
  parameter (three for three). So index 0 is `Squeeze`, whose default (on) is
  right if it can never be reached. `--names` and `verify.sh`'s oxbow step
  assert it.
- **The two pitches share one mapping**, so equal slider positions are equal
  pitches bit for bit and `PitchRatio` is exactly 1.0.
- **Output alpha is the seen picture's**: the mix of A's and B's alpha by the
  B fraction; the ridge highlight is composited OVER as white of coverage h. At
  Ridge Shine 0 the alpha is the sampled input's, untouched (`--ends`).
- **No clock, no buffer, no state across frames.** A resize has nothing to
  clear. `SetTime` is not overridden.
- **Predictions are made from what the harness ASKED** (`asked()`: the params
  it set, through `Controls.h`), never from `SetupForTest()`, which would follow
  a fault instead of catching it.
- `SetParamInfo` clamps a STANDARD default into 0..1, so every ranged parameter
  is 0..1 and the conversions live in `Controls.cpp`.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin at all.
- `lenticular_core` is an OBJECT library, not STATIC — the plugin registers
  itself from a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- GLSL reserved words to avoid: `patch sample input output filter common active
  half layout flat packed smooth round`, and the rest of GLSL 4.10's list.
  MSVC: no `M_PI` (use `kPi`), include `<cmath>`, never `near`/`far`.
- `demo/` is the browser demo at lenticular-demo.stoatworks-labs.com: the
  plugin's two shaders unedited (spliced from `Shaders.cpp` by script), and
  Controls.cpp, Lens.h's constants and `ProcessOpenGL`'s uniform arithmetic
  hand-ported in `demo/plugin.js`. A **mixer**: A is the kit's clip, B a second
  generated clip (the transport's `Clip B`). `demo/vendor/` is the shared kit --
  do not edit it; it is copied in by `stoatworks-backend/resolume-demo/sync.sh
  lenticular`. Serve with `python3 -m http.server` in `demo/`; deploy from the
  repo root with `cf-run npx wrangler deploy` (no build step; the host is a
  Worker ROUTE plus a proxied AAAA `100::` record, not a custom domain);
  `.github/workflows/deploy.yml` also ships it on every push to main that
  touches more than docs. Change a shader, Controls.cpp, Lens.h or
  ProcessOpenGL and the demo needs the same change -- the checker catches only
  the shaders. See AGENTS.md, "The browser demo".
- FFGL id is `LN01`. Display name `SW Lenticular`.
- **The OpenFX build** (`source/ofx/LenticularOFX.cpp`): identifier
  `com.stoatworks.lenticular`, label `Lenticular`, group `Stoatworks`, bundle
  id `com.stoatworks.lenticular.ofx` — permanent, saved projects refer to them,
  as do the parameters' camelCase script names. **Transition context only**:
  SourceFrom = A, SourceTo = B, the host's `Transition` = Opacity. Two
  OpenFX-only params after the rest, group Ends: `ends` (choice, **Fade 0 —
  the default — / Cut 1**; the order is saved by index) and `endLength`
  (0..0.5 of the transition, 0.15). Fade: exactly SourceFrom at 0 and
  SourceTo at 1 (`isIdentity`, plus a same-format pixel copy), a smoothstep
  crossfade with the card over each End Length (`card::CardStrength`,
  `card::Plain`, no GLSL twin). Cut: the card alone, the FFGL behaviour, bit
  for bit. No Filter
  (nothing to flip to) and no General (a different product, and in Resolve
  presumably a second entry among the clip effects): Nuke does not list it, by
  decision.
- **`source/Card.{h,cpp}` is shared by both builds**, GL-free, in the
  `lenticular_card` OBJECT library: the host values and their defaults (the
  FFGL constructor and the OFX descriptor read the same struct), the setup and
  the uniforms (`ProcessOpenGL` calls them), and `card::Shade`, the card
  shader's per-pixel stage in C++. **Every `//= mirrored` function there has a
  twin of the same name in `kLenticularShader`: change one, change both, run
  `lntest --cpu`** — and copy the shader into `demo/plugin.js` (the markers
  are part of its text).
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are GENERATED by the backend's
  `sync-about.py` and `sync-attributions.py`: never hand-edit them.
- The user guide is `docs/USER-GUIDE.md`, the only copy anyone edits;
  `docs/USER-GUIDE.pdf` and the site page are built from it by the website's
  `scripts/build_guides.py lenticular`.

## Not done yet
- **Never loaded into Resolume on macOS.** On Windows (Arena 7.27.1, llvmpipe)
  it was probed by REST + its log, and no frame of its picture there has been
  captured.
- **The OpenFX transition has been in Resolve, never in Vegas.** It matches
  the GPU per pixel on its own (`--cpu`) and through a test OFX host with a
  Transition context (1 of 255), and DaVinci Resolve Studio 21.1 (macOS, Edit
  page, 2026-10-04) rendered it byte-identical to that host at Transition
  (n + ½) / N — so under Fade Resolve's first and last transition frames keep a
  trace of the card (AGENTS.md "In DaVinci Resolve"). Vegas's marshalling, and
  any Windows or Linux host, are unseen.
- No presets. The browser demo exists; it is a port, not the plugin.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume). It records the host, the first frame's input sizes and paddings, the
first 16 changes of Opacity (what drove it), and each guard once.

    ~/Library/Logs/lenticular/lenticular.YYYY-MM-DD.log
