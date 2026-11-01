<p align="center"><img src="docs/splash.png" width="500" alt="neXt-Ray x64 / DX12"></p>

<p align="center"><a href="https://ko-fi.com/ozicom23"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="Support me on Ko-fi"></a></p>

# neXt-Ray Engine

A performance engine for [G.A.M.M.A.](https://github.com/Grokitach/Stalker_GAMMA): more fps, fewer stutters, faster
loading and less memory.

**The game looks and plays exactly like GAMMA.** Optimizations keep the same image and are on by default; anything that
trades image quality is an option. Mods stay as their authors made them.

Based on [Modded Exes](https://github.com/themrdemonized/xray-monolith), the engine of GAMMA's current release.
Everything Modded Exes has is still here.

## Highlights

### DirectX 12
<!-- screenshot: DX11 vs DX12, performance overlay in the same spot -->
A Direct3D 12 renderer next to the DX11 one (`AnomalyDX12AVX.exe`), with HDR10 output that keeps GAMMA's look.

### Upscaling: DLSS, FSR, XeSS
<!-- screenshot: native vs upscaled, scope lens sharp -->
NVIDIA DLSS 4, AMD FSR 3.1 and Intel XeSS. Scope lenses, night vision, the gas mask and the PDA are drawn after the
upscaler, so they stay sharp. Video > Advanced.

### Frame generation
<!-- screenshot: performance overlay, frame generation off / on -->
FSR or XeSS frame generation on top of the upscaler, on any GPU that runs them. On DX12, FSR's runs on its own GPU
queue.

### Texture streaming
<!-- screenshot: performance overlay VRAM line, before / after -->
Textures load as they are needed instead of all at once: texture memory at full quality 6.25 GB -> about 2.6 GB.

### General optimization
<!-- before / after screenshots with the performance overlay, same spot:
| Modded Exes | neXt-Ray |
|---|---|
| ![before](docs/img/before.png) | ![after](docs/img/after.png) | -->
Hundreds of smaller changes that add up, all with the same image:
- **Rendering:** hidden objects and shadows skipped, far and lamp shadows reused between frames, trees drawn in batches.
- **Lua scripts:** the garbage collector works in the background (GAMMA's biggest source of hitches), a faster JIT and
  faster calls into the engine.
- **A-Life:** squads coming online and their updates spread over several frames: no freeze when a squad appears.
- **GAMMA's mods:** the most expensive scripts and shaders made cheaper without touching their files.

### Fade to black transitions
<!-- video: a level change, cold (first time) and warm (files cached) -->
Level changes behind a short fade to black instead of the loading screen, with the next level read ahead near its level
changer: about a third less time per level change.

## More
- [Features](docs/FEATURES.md): everything added, with the console variables and the numbers.
- [Development](docs/DEVELOPMENT.md): building, testing, the rules for changes.

## License

The X-Ray engine source is GSC Game World's, under its [license](License.txt): non-commercial use only. Third-party
code keeps its own license (`src/3rd party`).
