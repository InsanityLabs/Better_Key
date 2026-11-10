# Better Key

A realtime GPU chroma/luma keyer for OBS Studio that doesn't suck.

Pure shader math — no AI models, no CPU readback, no inference threads.
Every pixel is keyed per-frame on the GPU with proper coverage estimation
and screen unmixing, so translucent content (particles, sheer fabric,
motion blur, smoke, glass) comes through with real alpha and real color
instead of getting hard-cut or turned green.

Built for VTubers, streamers, and anyone whose chroma key should have
worked better a decade ago.

## Why it's better than a normal chroma key

- **Coverage, not a cut.** Each pixel gets an estimated screen-fraction
  alpha, not an on/off mask — so soft edges, translucency, and particles
  survive.
- **Real unmixing.** Instead of just despilling green pixels, it inverts
  the compositing equation `c = fg·α + screen·(1−α)` to recover the
  foreground color underneath the screen bleed.
- **Translucent color fix.** Uncertain unmixed hues get pulled toward
  neutral so particle overlays read clean instead of shifting
  purple → yellow → green.
- **Auto-calibrate.** One click samples your frame's corners, finds the
  screen color, and derives similarity from the screen's own variance.
- **Luma assist.** Key dark or bright backgrounds alongside chroma.
- **Full matte output.** View/record the generated alpha matte itself —
  which stock chroma key doesn't even expose.
- **Realtime.** All pixel-shader work at full frame rate. ~0 added
  latency, tiny GPU cost.

## Install

Download `better-key-windows-x64.zip` from Releases, then either:

- Run `install.ps1` (right-click → Run with PowerShell), or
- Manually copy the `better-key` folder into
  `%PROGRAMDATA%\obs-studio\plugins\`

Restart OBS. Add **Filters → Better Key** to your source.

## Controls

| Control | What it does |
|---|---|
| Green/blue screen assist | Master switch for chroma keying |
| Screen color | The key color (green, blue, whatever your screen is) |
| Screen similarity | How close to the key color counts as screen |
| Screen smoothness | Edge transition softness |
| **Auto-calibrate screen** | Samples the frame corners, sets color + similarity for you |
| Luma assist | Off / Key dark background / Key bright background + threshold + softness |
| Despill strength | Removes screen spill AND cuts the faint background residue |
| Translucent color fix | Desaturates uncertain unmixed hues on partial-alpha pixels |
| Gamma / Contrast / Brightness / Opacity | Output color grade |
| Output | Composite (keyed) / Final alpha matte / Raw input |

## How it works

For every pixel the shader computes:

1. **Chroma distance** in Cb/Cr space (9-tap box filter, sRGB-aware —
   same math as OBS's chroma_key_filter_v2) → how "screen-like" the
   pixel is.
2. **Spill estimate** — how much of the key's dominant channel the pixel
   carries → screen coverage `α = 1 − spill`.
3. **Unmix** — solve `c = fg·α + screen·(1−α)` for `fg` to get the true
   foreground color back.
4. **Luma assist** multiplies in a luminance key if enabled.
5. Translucent desat → despill → color grade → premultiplied RGBA out.

That's the whole keyer. ~60 lines of math at display resolution.

## Building from source

Requirements: Windows x64, MSVC 2022, CMake ≥ 3.28.

```powershell
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

Output lands in `build_x64/rundir/RelWithDebInfo/better-key/`.
Copy that folder into `%PROGRAMDATA%\obs-studio\plugins\` or run
`installer/package.ps1` to produce the release zip.

## License

Custom license based on PolyForm Noncommercial 1.0.0 — see [LICENSE](LICENSE).

Short version: **content creators can use it commercially** (streams,
VTuber, videos, monetized content — all fine). What you can't do is
repackage/resell/relicense the software or lift the algorithm into
another product. Everyone else: noncommercial use only.

## Roadmap

- Residue-floor control for imperfect screens
- Multi-color screens / uneven lighting
- FFmpeg/OFX ports so this works on recorded footage, not just live
- Optional AI matte assist (once there's training data worth using)
