# LMU emulated quad-views foveation (branch `lmu-qvfr-v2`)

Design summary lives at the top of `src/d3d11/d3d11_foveation.h`; this file
is the operating guide.

## Requirements

- dxbc-spirv fork submodule (adds the `ViewIndex` built-in).
- Vulkan 1.1 `multiview` and 1.2 `shaderOutputViewportIndex` (Nvidia, RDNA2+).
  Without them the startup log says `LMU-FOVEATION: inactive (...)`.
- For gaze: the OpenXR layer (`~/repos/openxr-gaze-daemon`) enabled with
  `XR_GAZE_ENABLE=1` in the game's launch options.

## Config (`dxvk.conf` in the game dir, or `DXVK_CONFIG`)

| key | meaning | default |
|---|---|---|
| `d3d11.lmuFoveation` | master switch | False |
| `d3d11.lmuFoveationFocusCrop` | sharp region, fraction of width/height around the gaze | 0.4 |
| `d3d11.lmuFoveationFocusScale` | focus render density relative to native (1.5 = 1.5x per axis, filtered down at the composite), clamped so `focusCrop * focusScale <= 1` | 1.0 |
| `d3d11.lmuFoveationPeripheryScale` | periphery resolution, fraction of native | 0.5 |
| `d3d11.lmuFoveationGaze` | drive the focus from `Z:\dev\shm\lmu_gaze` | False |
| `d3d11.lmuFoveationFocusInset` | per-eye nasal inset (NDC) when gaze is unavailable | 0.25 |
| `d3d11.lmuFoveationFeather` | focus edge feather, fraction of the rect | 0.15 |
| `d3d11.lmuFoveationFragCoordRemap` | remap fragment SV_Position to true screen pixels | True |
| `d3d11.lmuFoveationCostGuard` | disable when `(focusCrop * focusScale)^2 + peripheryScale^2` reaches 1 | True |
| `d3d11.lmuFoveationDebug` | green outline on the focus rect | False |

GPU cost is roughly `(focusCrop * focusScale)^2 + peripheryScale^2` of a native eye for the
per-pixel stages, plus the geometry drawn twice (one pass, two views).
`peripheryScale = 1.0` disables foveation entirely (nothing to shrink).

## Post-build test plan

Set `lmuFoveation = True`, `Gaze = True`, `Debug = True`, launch with
`XR_GAZE_ENABLE=1`, keep `tools/lmu_gazewatch` running.

1. Startup log: `LMU-FOVEATION: enabled, focus crop ..., periphery scale ...,
   gaze yes, fragcoord remap yes`, then `detected eye target`, two
   `created lmu-shadow-*` lines, and later `created lmu-depth-upscaled`.
2. Main menu and loading screens: unchanged (identity push on non-foveated
   binds). Any scaling or offset here = regression.
3. Cockpit, daytime: green box follows the eyes and sits on the gaze at all
   four screen edges (clipped, never re-centred); focus sharp, periphery
   soft; no dark strip beside the box; no grid at
   `focusCrop 0.25 / peripheryScale 0.5`.
4. Cockpit and replay external camera, night: shadows and reflections on
   the car and road present and correctly placed. This is the
   `FragCoordRemap` check. Then set `lmuFoveationFragCoordRemap = False`
   and repeat: shadows/reflections should now be wrong or missing in the
   periphery. Restore to True.
5. Replay external camera, night, grazing road/grass: no static per-eye
   seam (the bilinear depth reconstruction).
6. `gazewatch`: a single live stream of `valid=1` samples while in the car,
   nothing before the session starts.
7. Frame time vs the old branch at the same settings; expect equal or
   better (no colour upscale).

## Not carried over from `lmu-qvfr`

Trace instrumentation, bisection toggles, colour upscale (colour consumers
are size-relative), copy-source remap (the game never copies from the eye
target), resolve-time depth fields. The debugging history is in that
branch's `PLAN.md`.
