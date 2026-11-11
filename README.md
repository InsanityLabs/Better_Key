# Better Key

A realtime GPU chroma/luma keyer for OBS Studio that doesn't suck.

Pure shader math. No AI models, no CPU readback, no inference threads.
Every pixel is keyed per-frame on the GPU with proper coverage estimation
and screen unmixing, so translucent content (particles, sheer fabric,
motion blur, smoke, glass) comes through with real alpha and real color
instead of getting hard-cut or turned green.

Built for VTubers, streamers, and anyone whose chroma key should have
worked better a decade ago.

## Why it's better than a normal chroma key

- **A learned screen, not a guessed color.** A 2058-point sample grid
  (49 x 42) watches the frame, finds the dominant screen-colored
  cluster, and fits a smooth per-region screen color. Every pixel is
  keyed against the screen color *where it is*, so uneven lighting and
  falloff stop producing haze. Live tracking keeps learning as your
  subject moves - no clean plate needed.
- **Coverage, not a cut.** Each pixel gets an estimated screen-fraction
  alpha, not an on/off mask - so soft edges, translucency, and particles
  survive.
- **Real unmixing.** Instead of just despilling green pixels, it inverts
  the compositing equation `c = fg*alpha + screen*(1-alpha)` to recover
  the foreground color underneath the screen bleed, with a
  neutral <-> vivid prior for the mathematically ambiguous cases.
- **Particle color repair.** Translucent pixels get noisy hue because
  the unmix divides by coverage. Repair borrows chroma from more solid
  neighbors and keeps each pixel's own brightness.
- **Auto-calibrate.** One click finds the dominant screen cluster,
  sets the key color, and derives similarity from the screen's own
  spread.
- **Luma assist.** Key dark or bright backgrounds alongside chroma.
- **Full matte output.** View/record the generated alpha matte itself -
  which stock chroma key doesn't even expose - or inspect the learned
  screen grid.
- **Realtime.** All pixel-shader work at full frame rate. ~0 added
  latency, tiny GPU cost.

## Install

Grab the latest `better-key-*-windows-x64.zip` from
[Releases](https://github.com/InsanityLabs/Better_Key/releases), then either:

- Run `install.ps1` (right-click -> Run with PowerShell), or
- Manually copy the `better-key` folder into
  `%PROGRAMDATA%\obs-studio\plugins\`

Restart OBS. Add **Filters -> Better Key** to your source.

## Controls

| Control | What it does |
|---|---|
| Green/blue screen assist | Master switch for chroma keying |
| Screen color | The key color (green, blue, whatever your screen is) |
| Screen similarity | How close to the key color counts as screen |
| Screen smoothness | Edge transition softness |
| **Auto-calibrate screen** | Samples the frame on a 2058-point grid, finds the dominant screen cluster, sets color + similarity |
| Live screen tracking | Keeps re-measuring the screen grid as your scene changes |
| Luma assist | Off / Key dark background / Key bright background + threshold + softness |
| Despill + haze cut | Removes screen spill AND cuts the faint background residue |
| Particle color: neutral <-> vivid | Prior for underdetermined translucent pixels: neutral is exact for white/grey/yellow, vivid for red/purple/blue |
| Particle saturation | Boosts saturation on translucent keyed pixels only (1 = unchanged) |
| Particle color repair | Borrows hue from more solid neighbors on low-coverage pixels |
| Desaturate translucent pixels | Pulls uncertain unmixed hues toward neutral |
| Gamma / Contrast / Brightness / Opacity | Output color grade |
| Output | Composite (keyed) / Final alpha matte / Raw input / Screen grid |

## How it works

Every 15 frames the filter samples the source on a 49 x 42 lattice
(2058 points), keeps the points in the dominant screen-colored cluster,
and fits a smooth quadratic surface per channel weighted toward the
purest samples - so particles drifting across the screen are ignored
instead of learned. That surface is uploaded as a small texture.

For every pixel the shader then computes:

1. **Local screen color** from the grid (or the picked key color if the
   grid hasn't converged yet).
2. **Chroma distance** in Cb/Cr space (9-tap box filter, sRGB-aware;
   same math as OBS's chroma_key_filter_v2) - how "screen-like" the
   pixel is.
3. **Unmix** - solve `c = fg*alpha + screen*(1-alpha)` for `fg` and
   `alpha`. The equation is one short, so a prior picks the coverage:
   neutral (key channel matches the larger other channel) for content
   near solid subject pixels, vivid (smallest in-gamut coverage) for
   free-floating translucent pixels.
4. **Particle repair** - low-coverage pixels borrow chroma from the 8
   more-solid sampled neighbors, keeping their own brightness.
5. **Residue cut** (despill) - trims faint background haze from the
   coverage estimate, after the color solve so it cannot shift hue.
6. **Luma assist** multiplies in a luminance key if enabled.
7. Translucent desat -> edge despill -> particle saturation -> color
   grade -> premultiplied RGBA out.

## Building from source

Requirements: Windows x64, MSVC 2022, CMake >= 3.28.

```powershell
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

Output lands in `build_x64/rundir/RelWithDebInfo/better-key/`.
Copy that folder into `%PROGRAMDATA%\obs-studio\plugins\` or run
`installer/package.ps1` to produce the release zip.

## License

Custom license based on PolyForm Noncommercial 1.0.0 - see [LICENSE](LICENSE).

Short version: **content creators can use it commercially** (streams,
VTuber, videos, monetized content - all fine). What you can't do is
repackage/resell/relicense the software or lift the algorithm into
another product. Everyone else: noncommercial use only.

## Roadmap

- FFmpeg/OFX ports so this works on recorded footage, not just live
- Optional AI matte assist (once there's training data worth using)
