# Attributions

Lenticular is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

It is a provisional hand copy in the shape `stoatworks-backend`'s
`scripts/sync-attributions.py` generates: Lenticular is not in the master lists yet.
When it is registered, the sync overwrites this file.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Mixer mechanics, two-input harness and pipe — Stoatworks genlock, wipe and relay

<https://github.com/stoatworks-labs/relay>  
Licence: MIT  
Copyright: Stoatworks Labs

The mixer mechanics of ProcessOpenGL (guard on the input count, guard on each pointer, one MaxUV per input, interleaved scoped bindings), the two-input harness rig, its --mixer check and its two-input --pipe with the cue sheet, the software-renderer pass and the CMake shape are genlock's, wipe's and relay's, adapted.

### Diagnostics log — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Diag.* is tinsel's log, by way of relay, renamed into this namespace.

### Sweep and verify shape — Stoatworks graticule

<https://github.com/stoatworks-labs/graticule>  
Licence: MIT  
Copyright: Stoatworks Labs

tools/sweep.py and the release-job-locally checks in tools/verify.sh follow genlock's, wipe's, relay's and graticule's.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned to b1afaf9 like the rest of the fleet.

The plugin ABI itself. An FFGL effect, source or mixer is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Lenticular printing

Implemented from the textbook optics of a lenticular sheet: a thin cylindrical lens focusing a parallel bundle at angle t onto its focal plane at f tan t, an interleaved print in that plane, the beat between two periodic structures as the moiré of misregistration, and a viewer at finite distance seeing each column at its own angle. No particular card, lens sheet, interleaving software or photograph of one was used; the ridge highlight, the edge shading and the residual magnification are looks, not a characterisation of any sheet.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
