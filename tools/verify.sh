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
#   build         a fresh universal Release build, which is what ships
#   suites        the plugin's claims, measured at TWO rasters: two inputs
#                 at two sizes with two MaxUVs, the card tilted all the way
#                 is A or B sampled at the lens pitch bitwise (alpha
#                 included), the whole card flips at the angles the lens
#                 geometry gives, a pitch mismatch bands at the moire period
#                 by FFT, a near viewer's flip sweeps across as D sin T,
#                 Opacity tilts the card monotonically, and one character
#                 of GLSL mutated
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
SUITES="names mixer ends flip moire distance opacity mutation"

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
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
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

step "bench"
"$LNTEST" --bench --frames 60 2>&1 | sed -n '3,8p' | sed 's/^/   /'

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
