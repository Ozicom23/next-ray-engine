<p align="center"><img src="docs/splash.png" width="500" alt="neXt-Ray x64 / DX12"></p>

# neXt-Ray Engine

A performance engine for [G.A.M.M.A.](https://github.com/Grokitach/Stalker_GAMMA): more fps, fewer stutters, faster
loading and less memory.

**The game looks and plays exactly like GAMMA.** Optimizations keep the same image and are on by default; anything that
trades image quality is an option. Mods stay as their authors made them.

Based on [Modded Exes](https://github.com/themrdemonized/xray-monolith), the engine of GAMMA's current release.
Everything Modded Exes has is still here.

## Highlights

### Upscaling and frame generation
<!-- screenshot: native vs DLSS/FSR quality, scope lens sharp after the upscaler -->
NVIDIA DLSS 4, AMD FSR 3.1 and Intel XeSS, with FSR or XeSS frame generation on top. Scope lenses, night vision, the
gas mask and the PDA are drawn after the upscaler, so they stay sharp. Pick them in Video > Advanced.

### DirectX 12 and HDR
<!-- screenshot: HDR on / off -->
A Direct3D 12 renderer next to the DX11 one, with HDR10 output that keeps GAMMA's look (Video > Basic). FSR frame
generation runs on its own GPU queue.

### Faster frames, same image
<!-- screenshot: performance overlay, before / after in the same spot -->
Hidden objects and shadows are skipped, far shadows and lamp shadows are reused between frames, and trees are drawn in
batches: about 13% less GPU time per frame and a fifth fewer draw calls.

### Fewer stutters
<!-- screenshot: frame time graph, before / after -->
Lua's garbage collector, GAMMA's biggest source of hitches, now works in the background while the frame renders. With
its last step in testing (`lua_parallel_gc_paced 2`), a benchmark flight gets 16% more fps, a 70% higher 1% low, and
the longest frame less than half as long.

### Less memory
Textures stream in as they are needed: texture memory at full quality 6.25 GB -> about 2.6 GB.

### Faster loading
Collision and script caches, a faster file index, the next level read ahead near its level changer, and level changes
behind a short fade: about a third less time per level change.

### Gamepad and gyro
<!-- screenshot: radial menu with gamepad prompts -->
Full gamepad play with a STALKER 2-like layout, a radial menu and button prompts. DualSense and DualShock 4 add gyro
aiming and adaptive triggers.

### Tools
<!-- screenshot: performance overlay detailed -->
A performance overlay (fps, CPU and GPU time, memory), Lua cost per mod, crash reports that name the mod at fault, a
watchdog that turns a frozen game into a crash report, and repeatable benchmark flights.

## Patches for GAMMA's mods
Some of GAMMA's own mods are made cheaper without changing what they do; their files stay untouched.

**Built in**, nothing to install:
- Dynamic Anomalies' 200-350 per-frame checks go through one dispatcher: 0.58 -> 0.07 ms per frame.
- Useful Idiots' emission cover cache, 3DSS's scope marker shaders and story item lookups no longer slow down loads:
  about 10% faster level changes.

Each patch applies only to the exact version of the mod it was made for. After that mod updates, the patch switches
itself off and the mod runs as it is. A file named `nextray_perf_off` in `appdata` turns them all off.

**The `neXt-Ray performance patches` MO2 mod** (the `mods` folder of the build): cheaper versions of GAMMA's
post-process shaders (AO, fog, SSR, debanding, terrain), the same image: about 0.5 ms less GPU time per frame, more
with a lot of sky in view. Copy it into MO2's mods folder, enable it and move it to the bottom of the mod list (highest
priority, so its shaders win). Everything else works without it.

## More
- [Features](docs/FEATURES.md): everything added, with the console variables.
- [Development](docs/DEVELOPMENT.md): building, testing, the rules for changes.

## License

The X-Ray engine source is GSC Game World's, under its [license](License.txt): non-commercial use only. Third-party
code keeps its own license (`src/3rd party`).
