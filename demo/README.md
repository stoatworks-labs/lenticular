# demo/ — the browser demo

Live at **<https://lenticular-demo.stoatworks-labs.com>**. Not served from this
README: `.assetsignore` keeps this file and `tools/` out of the upload.

    index.html       the shell
    plugin.js        the parameters, the plugin's two shaders, the ported conversions and uniforms
    vendor/          the shared kit, copied in by sync.sh — DO NOT EDIT
    tools/           check_shaders.py, run by tools/verify.sh
    _headers         CSP and caching, honoured by the Cloudflare assets runtime

## What this page is, exactly

A **port**, not a recording and not the plugin.

The shaders are the plugin's, copied across unedited by script: `VERTEX` and
`LENTICULAR` in `plugin.js` are `kVertexShader` and `kLenticularShader` from
`source/Shaders.cpp`. `tools/check_shaders.py` compares them character for
character and `../tools/verify.sh` runs it.

The CPU half is small and a port: Controls.cpp (every `...FromParam` and
`ParamFor...`), Lens.h's constants, and the arithmetic of
`Lenticular::ProcessOpenGL` that turns the parameters into the card's uniforms.
The plugin is stateless — no clock, no buffer — so there is no frame logic. On
2026-09-25 the page's output was compared with `lntest --pipe` fed the page's
own A and B at 640x360, twelve cases (two clip pairs, six settings): all within
1/255 per channel, and a one-step parameter mismatch fails clearly
(AGENTS.md, "The browser demo"). Outside those settings only a reader checks it.

Lenticular is a **mixer**, and the kit hands a demo one input. So A (the layer
below) is the kit's clip, relabelled `Clip A`, and B (this layer) is a second
copy of the kit's clip generator, picked by the transport's `Clip B`. Both are
the kit's generated test clips, not Resolume's demo footage. In Resolume the
tilt is the layer's opacity fader; here it is the `Opacity` slider, and "Rock
the card" in the transport — the page's, not a plugin control — moves it 1 → 0
→ 1 every six seconds until you touch it.

Everything else is not the plugin: no Resolume, no layer stack, no FFGL, no
padded textures (both MaxUVs are 1), and GLSL ES 3.00 in WebGL2 rather than
desktop GL 4.1 core. The page's own disclosure lists every difference.

## Working on it

```bash
python3 -m http.server 8952          # from this directory
python3 tools/check_shaders.py       # the copies still match the C++
../tools/verify.sh                   # everything, including the above
```

There is no build step. It is hand-written ES modules and what is committed is
what is served. A push to main deploys it (`.github/workflows/deploy.yml`);
by hand, `cf-run npx wrangler deploy` from the repo root. Verify by content:
`curl -s 'https://lenticular-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`.

URL parameters beyond the kit's: `clipb=<id>` picks B; `rock=0` stops the
rocking (in `embed=1` mode it is off unless `rock=1`).

**After changing a shader in `source/Shaders.cpp`, copy it across here too** —
`check_shaders.py` names the shader and the first differing line. After
changing Controls.cpp, Lens.h or ProcessOpenGL, change `plugin.js` to match;
nothing will tell you if you forget.
