#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
# Each check answers a question none of the others can:
#
#   shaders       does the shader compile, through a real GLSL compiler,
#                 before a host has to find out. A shader that will not
#                 compile presents to an operator as "the mixer does
#                 nothing", with the real message buried in the log.
#   demo          the browser demo's copies of the shaders are the plugin's,
#                 character for character (demo/tools/check_shaders.py). The
#                 page claims to run this plugin's GLSL; nothing else would
#                 notice if the two copies drifted apart
#   build         a fresh universal Release build, which is what ships
#   suites        the plugin's claims, measured at TWO rasters: two inputs
#                 at two sizes with two MaxUVs, the card tilted all the way
#                 is A or B sampled at the lens pitch bitwise (alpha
#                 included), the whole card flips at the angles the lens
#                 geometry gives, a pitch mismatch bands at the moire period
#                 by FFT, a near viewer's flip sweeps across as D sin T,
#                 Opacity tilts the card monotonically, one character
#                 of GLSL mutated, the OpenFX build's CPU twin of the
#                 shader against the GPU per pixel (--cpu), and the OpenFX
#                 build's end ramp (--fade, no GL)
#   software      the same suites on Apple's software renderer, which is
#                 what a GPU-less CI runner gets: a check calibrated on this
#                 Mac's GPU fails here before it fails in CI
#   sweep         does every control change the picture
#   pipe          the two-input filming mode: N frames in, N out; a ramp
#                 that ramps and a boolean that steps; and a closed stdout
#                 is exit 1, not a SIGPIPE death
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop
#                 while still producing a bundle that loads and exports
#                 plugMain
#   lipo          is the macOS build really universal, or did CMake latch
#                 the architecture list before -DCMAKE_OSX_ARCHITECTURES
#                 arrived and report success anyway
#   plist         does CFBundleExecutable name the binary that is actually
#                 on disk -- if it does not, codesign reports "code object
#                 is not signed at all" about a *nested* object and mentions
#                 neither the plist nor the cause
#   codesign      the exact command the release job runs, against a copy
#   oxbow         the name, id and TYPE a host sees. The only thing that
#                 makes this a mixer rather than an effect is the eighth
#                 argument of a macro, nothing in the build would notice if
#                 it were wrong, and a mixer that registered as an effect
#                 would be handed one input and return FF_FAIL for ever.
#                 The input count is a separate declaration and is asserted
#                 separately; so is parameter 0, which Arena hides.
#   openfx        the OpenFX bundle: CFBundleExecutable names the binary on
#                 disk, it ad-hoc signs (the release step), it exports
#                 OfxGetPlugin, it is universal, and a real OFX host loads it
#                 and describes it as a Transition. ofxprobe -- the fleet's
#                 probe host -- instantiates the Filter context only, so it
#                 cannot RENDER a transition; the per-pixel claim is --cpu's.
#   bench         the render cost, for the record. Not pass/fail -- there is
#                 no threshold worth asserting on somebody else's GPU -- but
#                 a verify run leaves a timing on the record, which is what
#                 turns "it feels slower" into a comparison.
#
# The last five are release-job work done locally on purpose. A check that
# only runs in CI, after a tag, is a check that will catch you after the tag.
#
set -uo pipefail

cd "$(dirname "$0")/.."

# resolume-ofx-bridge, for ofxprobe. It sits beside this repo's checkout --
# and from a git worktree `..` is the worktrees folder, not Projects/resolume,
# so the main checkout is found through git's common dir as well.
# LENTICULAR_BRIDGE overrides both, and OFXPROBE the probe itself.
BRIDGE="${LENTICULAR_BRIDGE:-}"
if [ -z "$BRIDGE" ]; then
	for candidate in "../resolume-ofx-bridge" \
	                 "$(dirname "$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null)")/../resolume-ofx-bridge"; do
		if [ -d "$candidate/build" ]; then
			BRIDGE="$candidate"
			break
		fi
	done
fi
BRIDGE="${BRIDGE:-../resolume-ofx-bridge}"

# oxbow, the FFGL test host. It sits beside this repo's checkout -- and from
# a git worktree `..` is the worktrees folder, not Projects/resolume, so the
# main checkout is found through git's common dir as well. OXBOW names the
# binary outright.
OXBOW_REPO=""
for candidate in "../oxbow" \
                 "$(dirname "$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null)")/../oxbow"; do
	if [ -d "$candidate/build" ]; then
		OXBOW_REPO="$candidate"
		break
	fi
done
OXBOW_REPO="${OXBOW_REPO:-../oxbow}"

BUILD="${BUILD:-build-universal}"
failures=0
LOGS="$( mktemp -d )"

# Every harness run instantiates the real plugin, and the plugin writes a
# log. Keep those lines out of the log an Arena session would be read from.
export LENTICULAR_LOG_DIR="${LENTICULAR_LOG_DIR:-$( mktemp -d )/lenticular-logs}"

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

#---------------------------------------------------------------------------
# The shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V,
# which demands an explicit layout( location ) on every uniform and varying.
# Those are Vulkan rules and not GLSL ones, and without the flag every shader
# "fails" for reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

# A shader may be several adjacent raw strings (MSVC caps one literal at about
# 16 KB), so everything up to the terminating semicolon is joined. There is
# one pass and it is well under the cap, but the joining stays, because the
# day a string grows past the cap is not the day to discover the extraction
# only ever read the first half of it.
named = {}
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(\w+)\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )

for name, body in named.items():
	if not ( body.lstrip().startswith( "#version" ) and "void main" in body ):
		continue
	# The vertex shader is the one that writes gl_Position; everything else is
	# a fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -lt 2 ]; then
		# Fewer than the two shaders this repo has is a FAILURE, not a pass.
		# It means the extraction above has lost track of where the GLSL
		# lives, and a check that silently looks at nothing is worse than no
		# check at all.
		printf '   only %d shaders were extracted -- the extraction has gone stale\n' "$n"
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

step "shaders"
if shaders_compile; then
	pass "every shader compiles"
else
	fail "a shader does not compile"
fi

#---------------------------------------------------------------------------
# The browser demo's shaders. demo/plugin.js carries a copy of every shader in
# source/Shaders.cpp; a copy that drifts still renders a plausible card, so
# only an exact comparison catches it.
#---------------------------------------------------------------------------
step "demo"
if python3 demo/tools/check_shaders.py >"$LOGS/demo.txt" 2>&1; then
	pass "$( tail -1 "$LOGS/demo.txt" )"
else
	sed 's/^/   /' "$LOGS/demo.txt"
	fail "the demo's shaders have drifted -- copy source/Shaders.cpp across into demo/plugin.js"
fi

#---------------------------------------------------------------------------
# A fresh universal Release build -- the one that ships. The dev build in
# build/ is arm64 only and is not what any of the binary checks below should
# be looking at.
#---------------------------------------------------------------------------
step "build"
if [ ! -d "$BUILD" ]; then
	cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
fi
if cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "universal Release build"
else
	fail "build failed -- run: cmake --build $BUILD"
	exit 1
fi

LNTEST="$BUILD/lntest"
SUITES="names mixer ends flip moire distance opacity mutation cpu fade"

step "suites"
for t in $SUITES; do
	if "$LNTEST" --$t >/dev/null 2>&1; then pass "lntest --$t"; else fail "lntest --$t"; fi
done

#---------------------------------------------------------------------------
# The same suites on Apple's SOFTWARE renderer, which is what GitHub's
# GPU-less macOS runner falls back to. Its interpolated uv is good only to the
# GL spec's 1 part in 10^5 (the GPU's is good to a float ULP), and wipe's
# --area failed in CI on exactly that before its tolerance allowed for it.
# Running here too means a check calibrated on this Mac's GPU fails on this
# Mac.
#---------------------------------------------------------------------------
step "software renderer"
if [ "$(uname)" = "Darwin" ]; then
	for t in $SUITES; do
		[ "$t" = names ] && continue
		[ "$t" = fade ] && continue
		if LNTEST_RENDERER=software "$LNTEST" --$t >/dev/null 2>&1; then
			pass "lntest --$t (software)"
		else
			fail "lntest --$t on the software renderer -- run: LNTEST_RENDERER=software $LNTEST --$t"
		fi
	done
fi

step "sweep"
if python3 tools/sweep.py --binary "$LNTEST" --size 480x270 >"$LOGS/sweep.txt" 2>&1; then
	pass "$( tail -1 "$LOGS/sweep.txt" )"
else
	echo "   *** dead controls, see $LOGS/sweep.txt"
	tail -4 "$LOGS/sweep.txt" | sed 's/^/   /'
	fail "tools/sweep.py reports a dead control"
fi

#---------------------------------------------------------------------------
# The pipe. Three things, each a way a filming run has gone wrong before:
# the frame count in equals the frame count out; a cue on an option or a
# switch STEPS on its frame and a standard parameter ramps; and a reader
# that closes stdout makes the harness exit 1 -- SIGPIPE is ignored, so it
# is not killed with 141 and does not report success either.
#---------------------------------------------------------------------------
step "pipe"
W=64; H=36; FRAME=$(( W * H * 4 ))
tmp=$( mktemp -d )
bytes=$( head -c $(( FRAME * 3 )) /dev/zero | "$LNTEST" --pipe --size ${W}x${H} 2>/dev/null | wc -c | tr -d ' ' )
if [ "$bytes" -eq $(( FRAME * 3 )) ]; then
	pass "3 frames in, 3 frames out ($bytes bytes)"
else
	fail "3 frames in gave $bytes bytes out, expected $(( FRAME * 3 ))"
fi
# Cues: Opacity ramps 0 -> 1 over frames 0..2 and Squeeze steps from on to
# off at frame 4. A is a flat 0x80 on stdin; B is the bars card, twelve
# bars with red 16 + 20 j. Four fat lenses of 16 px, an ideal lens, the
# viewer at infinity: the first pixel is lens 0's sample, so it says which
# picture and which bar the card showed.
#   frame 0    Opacity 0    -> A, 128                                   A
#   frame 1    Opacity 0.5  -> square on: B at the strip's start, bar 0 0
#   frame 2,3  Opacity 1    -> B squeezed: mid-lens, bar 1              1 1
#   frame 4,5  Squeeze off  -> B unsqueezed: three-quarter lens, bar 2  2 2
# An Opacity that STEPPED would hold 0 at frame 1 (A); a Squeeze that
# RAMPED would be 0.5 at frame 2 and off there (bar 2 a frame early).
printf '0 Opacity 0\n2 Opacity 1\n0 Squeeze 1\n4 Squeeze 0\n' > "$tmp/cues.txt"
head -c $(( FRAME * 6 )) /dev/zero | LC_ALL=C tr '\000' '\200' | "$LNTEST" --pipe --size ${W}x${H} --input-b bars \
	--set "Lens Pitch=0" --set "Interleave Pitch=0" --set "Distance=1" --set "Focus Spot=0" \
	--set "Bleed=0" --set "Ridge Shine=0" --script "$tmp/cues.txt" 2>/dev/null > "$tmp/out.rgba"
firsts=$( python3 - "$tmp/out.rgba" $FRAME <<'PY'
import sys
d = open( sys.argv[ 1 ], "rb" ).read(); n = int( sys.argv[ 2 ] )
def name( v ):
	if v == 128: return "A"
	return str( ( v - 16 ) // 20 ) if v >= 16 and ( v - 16 ) % 20 == 0 else "?"
print( " ".join( name( d[ i * n ] ) for i in range( len( d ) // n ) ) )
PY
)
if [ "$firsts" = "A 0 1 1 2 2" ]; then
	pass "cues: Opacity ramps, Squeeze steps ($firsts)"
else
	fail "cues gave '$firsts', expected 'A 0 1 1 2 2'"
fi
head -c $(( FRAME * 3 )) /dev/zero | "$LNTEST" --pipe --size ${W}x${H} 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[1]}
if [ "$status" -eq 1 ]; then
	pass "a closed stdout is exit 1 (SIGPIPE ignored)"
else
	fail "a closed stdout gave exit $status, expected 1"
fi
rm -rf "$tmp"

BUNDLE="$BUILD/Lenticular.bundle"
BIN="$BUNDLE/Contents/MacOS/Lenticular"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under
	# `set -o pipefail`: grep exits at once, nm takes SIGPIPE, and the
	# pipeline reports that failure. Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ "$ident" = "com.stoatworks.ffgl.lenticular" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident'"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Lenticular.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	# The name, id, type and input count as a HOST reads them. A bundle can
	# build, export plugMain and still register itself under the wrong name
	# or -- the one that matters here -- the wrong TYPE, and nothing else in
	# this script would notice.
	OXBOW="${OXBOW:-$OXBOW_REPO/build/oxbow}"
	if [ -x "$OXBOW" ]; then
		out=$("$OXBOW" probe "$BUNDLE" 2>&1)
		case "$out" in
			*"name:        SW Lenticular"*) pass "the host sees the name SW Lenticular" ;;
			*) fail "wrong or missing name -- see: $OXBOW probe $BUNDLE" ;;
		esac
		case "$out" in
			*"id:          LN01"*) pass "the host sees the id LN01" ;;
			*) fail "wrong or missing id" ;;
		esac
		case "$out" in
			*"type:        mixer"*) pass "the host sees a MIXER" ;;
			*) fail "wrong plugin type -- FF_MIXER is the 8th argument of CFFGLPluginInfo" ;;
		esac
		case "$out" in
			*"inputs:      2..2"*) pass "the host is told to give it two inputs" ;;
			*) fail "wrong input count -- SetMinInputs/SetMaxInputs are separate from the type" ;;
		esac
		# Arena does not expose a mixer's parameter 0 (measured on genlock,
		# wipe and relay), so index 0 is sacrificial and must be a control
		# whose default is right for ever. This reads the declaration; only
		# Arena can say which one it hides.
		case "$out" in
			*"[ 0] Squeeze          type=0   default=1 "*) pass "parameter 0, which Arena hides, is Squeeze, a boolean, on" ;;
			*) fail "parameter 0 is not Squeeze (boolean, on) -- Arena hides a mixer's first parameter" ;;
		esac
		# The tilt is named Opacity, and the name is load-bearing: Arena
		# binds a mixer parameter of that name to the layer's fader.
		case "$out" in
			*"] Opacity          type=10"*) pass "the tilt is a standard parameter named Opacity" ;;
			*) fail "no standard parameter named Opacity -- Arena binds the layer fader by that name" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

#---------------------------------------------------------------------------
# The OpenFX bundle. cmake/InfoOFX.plist.in is copied from repo to repo, and a
# copy with the previous plugin's name in CFBundleExecutable does not fail the
# build: it fails at RELEASE time, in codesign, with a message that names a
# "subcomponent" and never the plist. So the plist is checked against the
# binary on disk and the release job's exact codesign is run, on a copy.
#---------------------------------------------------------------------------
OFXB="$BUILD/Lenticular.ofx.bundle"
if [ "$(uname)" = "Darwin" ]; then
	step "openfx"
	if [ ! -d "$OFXB" ]; then
		fail "no OpenFX bundle at $OFXB (configured with -DBUILD_OFX=OFF?)"
	else
		OFXBIN="$OFXB/Contents/MacOS/Lenticular.ofx"
		named=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$OFXB/Contents/Info.plist" 2>/dev/null)
		if [ -n "$named" ] && [ -f "$OFXB/Contents/MacOS/$named" ]; then
			pass "CFBundleExecutable ($named) is on disk"
		else
			fail "CFBundleExecutable is '$named' but no such binary is in the bundle"
		fi
		ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$OFXB/Contents/Info.plist" 2>/dev/null)
		if [ "$ident" = "com.stoatworks.lenticular.ofx" ]; then
			pass "CFBundleIdentifier is $ident"
		else
			fail "CFBundleIdentifier is '$ident'"
		fi
		tmp=$(mktemp -d)
		cp -R "$OFXB" "$tmp/"
		if codesign --force --sign - --timestamp=none "$tmp/Lenticular.ofx.bundle" >/dev/null 2>&1; then
			pass "ad-hoc signs (the command the release job runs)"
		else
			fail "the OpenFX bundle will not codesign"
		fi
		rm -rf "$tmp"
		# Captured and matched, not piped into grep -q: see "registration".
		syms=$(nm -gU "$OFXBIN" 2>/dev/null)
		case "$syms" in
			*_OfxGetPlugin*) pass "exports OfxGetPlugin" ;;
			*) fail "no OfxGetPlugin -- no host will see a plugin" ;;
		esac
		archs=$(lipo -archs "$OFXBIN" 2>/dev/null)
		case "$archs" in
			*arm64*x86_64*|*x86_64*arm64*) pass "universal ($archs)" ;;
			*) fail "not universal (got: $archs)" ;;
		esac

		# A real OFX host's loader and describe actions. The probe cannot
		# instantiate a Transition, so it reports this plugin as unusable --
		# for Resolume, which is what it was written to answer -- and that
		# line is expected. What it proves is that the binary loads, its
		# static initialisers and factory run, and it describes itself under
		# the identity a saved project will refer to.
		OFXPROBE="${OFXPROBE:-$BRIDGE/build/ofxprobe}"
		if [ -x "$OFXPROBE" ]; then
			out=$("$OFXPROBE" --dir "$BUILD" 2>&1)
			case "$out" in
				*"com.stoatworks.lenticular"*"label      : Lenticular"*) pass "ofxprobe loads com.stoatworks.lenticular, labelled Lenticular" ;;
				*) fail "ofxprobe does not find com.stoatworks.lenticular -- run: $OFXPROBE --dir $BUILD" ;;
			esac
			case "$out" in
				*"grouping   : Stoatworks"*) pass "in the Stoatworks group" ;;
				*) fail "not in the Stoatworks group" ;;
			esac
			case "$out" in
				*"contexts   : OfxImageEffectContextTransition "*) pass "described as a Transition, and nothing else" ;;
				*) fail "the contexts are not exactly Transition -- see: $OFXPROBE --dir $BUILD" ;;
			esac

			# And a RENDER through the host, when the probe can host a
			# Transition (the test host's --context; the bridge's
			# origin/main probe cannot). The host's own SourceFrom,
			# SourceTo and Transition marshalling, against the FFGL
			# plugin's GPU render of the same pictures (lntest --pipe):
			# the comparison --cpu makes, with a real host in the middle.
			# Opaque cards, so the 8-bit RGB the host's PPM carries is the
			# whole picture.
			#
			#   Ends = Cut, the FFGL behaviour, against the GPU. Tolerance
			#     4 codes: --cpu's derived bound at these settings (the
			#     filter's 8-bit weights on a full-scale step, 2.0 codes,
			#     the ridge at Shine 0.3, 1.4, and float) rounded up to
			#     whole codes. The control -- the host at 0.47 against the
			#     GPU at 0.53 -- must miss it.
			#   Ends = Fade, the default: Transition 0 is SourceFrom and 1
			#     is SourceTo byte for byte, rendered and through
			#     isIdentity; mid-ramp the picture is ( 1 - s ) plain + s
			#     card within the same 4 codes (the card's bound scaled by
			#     s < 1, plus the composite's rounding); between the ramps
			#     it is the Cut render's bytes.
			help=$("$OFXPROBE" --help 2>&1)
			case "$help" in
				*"--context"*)
					if python3 - "$OFXPROBE" "$BUILD" "$LNTEST" >"$LOGS/ofxrender.txt" 2>&1 <<'OFXRENDER_PY'
import os, subprocess, sys, tempfile
probe, build, lntest = sys.argv[1:4]
W, H = 320, 180
BARS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255), (40, 40, 40)]
def card(kind):
	out = bytearray()
	for y in range(H):
		for x in range(W):
			u, v = (x + 0.5) / W, (y + 0.5) / H
			if kind == "a":
				c = BARS[min(7, int(u * 8))]
				if v >= 0.75 and u < 0.5:
					k = ((x + y) & 1) * 255
					c = (k, 255 - k, k)
			else:
				c = (240, 140, 40) if int((u * 1.6 + v) * 8) & 1 else (245, 195, 105)
				if u < 0.3 and 0.15 < v < 0.85:
					c = (35, 30, 55)
			out += bytes(c)
	return bytes(out)
tmp = tempfile.mkdtemp()
cards = {}
for k in "ab":
	rgb = card(k)
	cards[k] = rgb
	open(os.path.join(tmp, k + ".ppm"), "wb").write(b"P6\n%d %d\n255\n" % (W, H) + rgb)
	rgba = bytearray()
	for i in range(0, len(rgb), 3):
		rgba += rgb[i:i + 3] + b"\xff"
	open(os.path.join(tmp, k + ".rgba"), "wb").write(bytes(rgba))
def host(t, sets):
	out = os.path.join(tmp, "host.ppm")
	cmd = [probe, "--no-system-dirs", "--dir", build, "--render", "com.stoatworks.lenticular", "--context", "transition",
	       "--from", os.path.join(tmp, "a.ppm"), "--to", os.path.join(tmp, "b.ppm"), "--transition", str(t), "--out-only", out]
	for s in sets:
		cmd += ["--set", s] if s != "--identity" else [s]
	r = subprocess.run(cmd, capture_output=True, text=True)
	# The instance must come from THIS build, not an installed copy.
	where = [l.split("instance from ", 1)[1].rsplit(" (", 1)[0] for l in r.stdout.splitlines() if "instance from " in l]
	if r.returncode != 0 or not where or os.path.realpath(os.path.dirname(where[0])) != os.path.realpath(build):
		print(r.stdout, r.stderr)
		sys.exit(1)
	data = open(out, "rb").read()
	return data[data.index(b"255\n") + 4:]
def gpu(t, sets):
	cmd = [lntest, "--pipe", "--size", "%dx%d" % (W, H), "--pipe-src", os.path.join(tmp, "b.rgba"), "--set", "Opacity=%s" % t]
	for s in sets:
		cmd += ["--set", s]
	r = subprocess.run(cmd, stdin=open(os.path.join(tmp, "a.rgba"), "rb"), capture_output=True)
	return bytes(r.stdout[i] for i in range(len(r.stdout)) if i % 4 != 3)
def diff(a, b):
	worst = max(abs(x - y) for x, y in zip(a, b))
	px = sum(1 for i in range(0, len(a), 3) if a[i:i + 3] != b[i:i + 3])
	return worst, px
TOL = 4
bad = 0
CUT = ["ends=1"]
cases = [(0, [], []), (0.47, [], []), (1, [], []),
         (0.4, ["lensPitch=0.6", "interleavePitch=0.58"], ["Lens Pitch=0.6", "Interleave Pitch=0.58"]),
         (0.55, ["squeeze=0", "distance=0.5", "ridgeShine=0"], ["Squeeze=0", "Distance=0.5", "Ridge Shine=0"])]
for t, ofx, ffgl in cases:
	w, px = diff(host(t, CUT + ofx), gpu(t, ffgl))
	print("Cut  Transition %-4s %-40s worst %d of 255, %d of %d px differ" % (t, " ".join(ofx) or "defaults", w, px, W * H))
	bad += w > TOL
w, px = diff(host(0.47, CUT), gpu(0.53, []))
print("control: the host at 0.47 against the GPU at 0.53: worst %d of 255, %d px differ" % (w, px))
bad += w <= TOL
# Fade, the default. The ends are the clips, byte for byte.
for t, k in ((0, "a"), (1, "b")):
	for how in ([], ["--identity"]):
		same = host(t, how) == cards[k]
		print("Fade Transition %s %-12s is %s: %s" % (t, " ".join(how) or "rendered", "SourceFrom" if k == "a" else "SourceTo", "byte-identical" if same else "DIFFERS"))
		bad += not same
# Mid-ramp: ( 1 - s ) plain + s card, s the smoothstep of End Length 0.15.
def strength(t, L=0.15):
	e = min(t, 1 - t)
	x = e / L
	return 1.0 if e >= L else x * x * (3 - 2 * x)
for t, k in ((0.06, "a"), (0.95, "b")):
	s = strength(t)
	got, card_ = host(t, []), gpu(t, [])
	want = bytes(int(round((1 - s) * p + s * c)) for p, c in zip(cards[k], card_))
	w, px = diff(got, want)
	print("Fade Transition %-4s s %.3f: worst %d of 255 from (1 - s) plain + s card, %d px differ" % (t, s, w, px))
	bad += w > TOL
same = host(0.47, []) == host(0.47, CUT)
print("Fade Transition 0.47, between the ramps: %s the Cut render" % ("byte-identical to" if same else "DIFFERS from"))
bad += not same
sys.exit(1 if bad else 0)
OFXRENDER_PY
					then
						pass "renders as a Transition in a real OFX host: Cut within 4 codes of the GPU ($( grep -c '^Cut' "$LOGS/ofxrender.txt" ) settings, control rejected); Fade's ends byte-identical to the clips, its ramp the crossfade"
					else
						sed 's/^/   /' "$LOGS/ofxrender.txt"
						fail "the OpenFX transition rendered through $OFXPROBE disagrees with the GPU"
					fi
					;;
				*) printf '   skipped: this ofxprobe cannot host a Transition (no --context); OFXPROBE=<a probe that can> to render one\n' ;;
			esac
		else
			printf '   skipped: ofxprobe not built at %s\n' "$OFXPROBE"
		fi
	fi
fi

step "bench"
"$LNTEST" --bench --frames 60 2>&1 | sed -n '3,8p' | sed 's/^/   /'

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
