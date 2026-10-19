# neXt-Ray Engine

A performance fork of the STALKER Anomaly engine for [G.A.M.M.A.](https://github.com/Grokitach/Stalker_GAMMA): upscaling,
frame generation, faster frames and loading, less memory. Aimed at the Steam Deck, and it helps every PC too.

The rule above all: **the game looks and plays exactly like GAMMA.** Optimizations keep the same image and are on by
default; anything that trades image quality is an option or a preset. Mods stay as their authors made them.

Based on [Modded Exes](https://github.com/themrdemonized/xray-monolith) (`all-in-one-vs2022-wpo-mt`, the engine of
GAMMA's current release, MT-TEST_2026.5.15). Everything Modded Exes has is still here.

## What's different from Modded Exes

### Upscaling (D3D11 and D3D12)
- **NVIDIA DLSS 4** Super Resolution and **AMD FSR 3.1** (vendored FidelityFX SDK, own D3D11 backend), in
  Video > Advanced. Only the upscalers the GPU supports are offered. `r4_upscaler`, `r4_upscaler_quality`,
  custom render scale `r4_upscaler_scale`, `r4_dlss_preset`, FSR 16-bit math `r4_fsr_fp16`.
- Textures keep the sharpness of the output resolution (mip LOD bias while upscaling).
- Gas mask, night vision and shader scopes are drawn after the upscaler, at output resolution
  (`r4_upscaler_late_overlays`), with the NVG blur at output resolution (`r4_upscaler_nvg_blur`).
- 3DSS scope lenses drawn after the upscaler (`r4_upscaler_scope_late`, `r4_upscaler_lens_patch`), no jitter while
  aiming through them (`r4_upscaler_jitter 2`), the PDA screen after it (`r4_upscaler_pda_late`).
- A reactive mask for lenses, foliage and first-person items (`r4_upscaler_reactive`), grass jitter fix
  (`r4_upscaler_grass_jitter`), history reset after loads and camera cuts.

### D3D12 renderer (`AnomalyDX12AVX.exe`)
- The R4 renderer on Direct3D 12 (`src/Layers/xrRenderDX12`, D3D12MA), with threaded command recording and a
  pipeline cache on disk, created ahead at the next start.
- **Intel XeSS upscaling** (`r4_upscaler xess`).
- **Frame generation**, AMD FSR 3.1 or Intel XeSS with XeLL (`r4_framegen off|fsr|xess`, "Frame Generation" in
  Video > Advanced; needs an upscaler on).
- Frame time smoothing (`r__frame_time_smoothing`).

### Frame time (same image, on by default)
- Hi-Z occlusion culling on the GPU (`r4_hiz_cull`): hidden trees, statics and dynamic objects skipped.
- Sun shadow caster culling (`r4_sun_caster_cull`), the three sun cascades culled in parallel (`r4_sun_parallel_cull`),
  far sun cascades kept between frames (`r4_sun_cascade_cache`), shadow map draw order (`r4_smap_order`).
- Lamp shadow maps kept between frames for lamps that nothing moves through (`r4_light_smap_keep`), lamp queries in
  parallel (`r4_lights_parallel_query`).
- Constant buffers uploaded and bound only when changed (`r4_cbuffer_skip_unchanged`, `r4_constants_per_frame`).
- Post-process ping-pong instead of copies (`r4_post_pingpong`), water SSR skipped with no water in view.
- Main view visibility in parallel (`r4_main_parallel_cull`, off by default), bones on all cores
  (`mt_calc_bones_parallel`).
- Waitable swap chain for lower input latency (`r4_swap_wait`, `r4_frame_latency`).
- Shader warm-up at the start (`r4_shader_warmup`) with a progress window (`r4_shader_progress`), and a counter while
  shaders compile during the game.
- Shader fixes (`r4_shader_patches`): early-outs and cheaper sampling in GAMMA's post-process shaders, same image.

Steam Deck, Cordon, FSR balanced: GPU ~22.6 -> ~19.7 ms per frame, main thread ~20.5 -> ~18.5 ms, draw calls
~5,600 -> ~4,400.

### Memory
- `texture_lod` really applied (D3DX11 ignored the mip skip), scope reticles always at full detail.
- Weather sky textures loaded on demand (`r4_sky_on_demand`).
- Texture streaming (`r4_tex_stream`): world textures load small with the level and in full in the background on
  first use; unused ones drop back after 60 s. UI, fonts, LUTs, sky and reticles are never streamed.

Texture memory on the Deck: 6.25 GB -> 2.5-2.8 GB at `texture_lod 0`. GPU memory in use: ~6.7 GB -> 2.3-3.6 GB.

### Loading
- Collision model cache (`appdata\cform_cache`), script bytecode cache, ini hash index.
- Native string and ini helpers for `_g.script`, Lua functions looked up by key instead of walking `_G`.
- File index on worker threads, script folders scanned once, 30 precache frames instead of 60.

Level transition Cordon -> Swamp on the Deck: ~26 s -> ~16.5 s.

### Patch mod: `mods/neXt-Ray performance patches`
An MO2 mod (shipped with the builds) for GAMMA's own mods, without changing what they do: Dynamic Anomalies' per-frame
closures through one dispatcher, Useful Idiots' emission cover cache, 3DSS marker shaders normalized once per weapon,
and the shader fixes. `appdata\nextray_perf_off` turns it off.

### Fixes
- Freeze with `mt_calc_bones_parallel 1` in crowded places (foot IK of two models deadlocked).
- Deadlock at a save's load between texture loading threads and GPU memory accounting.
- Recursive `file_list_open_ex` dropped loose files in subfolders.
- Wine/macOS: ImGui windows scaled to the back buffer when the window's client area has another size.

### Tools
- Performance overlay (`perf_overlay`): FPS over time windows, CPU/GPU/engine times, a CPU- or GPU-bound hint, VRAM and RAM.
- Profilers: `stat_gpu_passes`, `stat_cpu_passes`, `stat_next_load`, `stat_script_load`, `stat_gpu_memory`,
  `stat_tex_stream`, `stat_hiz`, `stat_sun_casters`, `stat_frame_pacing <s>` (every frame to a CSV in `logs/`).
- Remote console (`-remote_console`), see below.
- `mouse_filter 1`: averages the mouse over two frames for uneven mouse input (macOS); off by default.

## Development

### Building

The build is the CI's (`.github/workflows/dev-build.yml`): configuration `DX11-AVX` builds both `AnomalyDX11AVX.exe`
and `AnomalyDX12AVX.exe`. A plain `msbuild` silently leaves out DLSS and FSR; they need:
1. the NVIDIA DLSS SDK (tag in `dev-build.yml`) in `src/3rd party/NVIDIA-DLSS`,
2. the FSR shaders: `src/3rd party/FidelityFX-FSR3/compile_shaders.ps1`,
3. the frame generation shaders: `compile_shaders_dx12.ps1 -Compiler <FidelityFX_SC.exe>` (FidelityFX SDK v1.1.4),
4. `msbuild /m /p:Configuration=DX11-AVX src/engine-vs2022.sln`.

Every push to `master` runs the dev build in GitHub Actions (~38 min) and uploads the package and the PDBs.

#### Local build on a Windows VM: `tools/vm-build/build.sh`

From macOS or Linux, the working tree is built on a Windows machine over SSH, incrementally (a one-line change
relinks in ~5 min, mostly LTCG):

```sh
tools/vm-build/build.sh                 # build the tree as it is (uncommitted files included)
tools/vm-build/build.sh --install       # build, then install into the local game folder
tools/vm-build/build.sh --deck          # build, then install on the Steam Deck over SSH
tools/vm-build/build.sh --install-to user@host:/path/to/gamma   # any game folder, local or SSH; repeatable
tools/vm-build/build.sh --package --fetch   # the CI artifact layout, copied to _build/vm/package
tools/vm-build/build.sh --rebuild       # full rebuild
```

- The tree is pushed as a snapshot commit; your index and branches are untouched.
- `build.ps1` does the CI steps on the VM: it fetches the DLSS SDK and recompiles the FSR shaders only when they changed.
- Install: the exes and `nvngx_dlss.dll` to `anomaly/bin` (the replaced ones go to `backup-nextray-exes-prev/`),
  `gamedata/` to `anomaly/gamedata`, the patch mod to `gamma/mods`. Close the game first.
- Environment: `NEXTRAY_VM` (default `windows@192.168.64.4`), `NEXTRAY_VM_DIR` (`C:/nr`), `NEXTRAY_GAME`, `NEXTRAY_DECK`.

VM setup (once): Windows 11 (ARM64 works, it builds x64 with the cross compilers), OpenSSH server, Git, VS 2022 Build
Tools with the C++ workload, FidelityFX SDK v1.1.4 in `C:\nr-deps\ffx-sdk`, and an SSH key for the Mac.

### Testing: remote console

Start the game with `-dbg -remote_console` (MO2: the executable's arguments). The engine then reads console commands
from `appdata/remote_cmd.txt` and writes its state twice a second to `appdata/remote_status.txt`
(`state=menu|level|loading|idle`, `fps`, `frame`, `queue`, `waiting`, `last`). Write the command file to a temporary name
and rename it, one command per line; the game deletes it when it has taken the commands.

Commands only for the remote console:
- `rc_wait <s>`: wait before the next commands,
- `rc_wait_level <s>`: wait until a level is loaded and running,
- `rc_clear`: drop the queued commands,
- `rc_silent_level_changers 1`: level changers travel without their dialog.

`tools/remote_console.sh` (Steam Deck; set `GAMMA_APPDATA` for another game folder):

```sh
tools/remote_console.sh launch              # start the game through Steam, wait for the main menu
tools/remote_console.sh load test           # load a save (no spaces in its name), wait until it runs
tools/remote_console.sh send "perf_overlay detailed" "stat_frame_pacing 20"
tools/remote_console.sh run commands.txt    # queue a file of commands
tools/remote_console.sh idle                # wait until the queue is done
tools/remote_console.sh transition x y z    # travel through a level changer, print the load phase times
tools/remote_console.sh status | log 50 | quit
```

Useful in tests: `screenshot`, `dbg_freeze 1` (time stops, rendering goes on), `vid_restart`, `run_string <lua>`
(about 240 characters at most), and the profilers above. A frozen game writes a hang dump (`logs/hang_*.txt`).

### Rules for changes
- No visual or behaviour change from GAMMA without an option; new optimizations must give the same image.
- Mods are not edited; fixes for them go into the patch mod.
- Don't name variables `far` / `near` (Windows macros).

## License

The X-Ray engine source is GSC Game World's, under its [license](License.txt): non-commercial use only. Third-party
code keeps its own license (`src/3rd party`).
