# Features

Everything neXt-Ray adds to Modded Exes, with the console variables. Defaults keep GAMMA's image; the
highlights are in the [README](../README.md).

## Upscaling (D3D11 and D3D12)
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

## D3D12 renderer (`AnomalyDX12AVX.exe`)
- The R4 renderer on Direct3D 12 (`src/Layers/xrRenderDX12`, D3D12MA), with threaded command recording and a
  pipeline cache on disk, created ahead at the next start.
- **Intel XeSS upscaling** (`r4_upscaler xess`).
- **Frame generation**, AMD FSR 3.1 or Intel XeSS with XeLL (`r4_framegen off|fsr|xess`, "Frame Generation" in
  Video > Advanced; needs an upscaler on).
- Frame time smoothing (`r__frame_time_smoothing`).
- FSR frame generation's interpolation on the async compute queue (`r4_framegen_async`, on): Deck +5% fps, half the
  queued time.
- **HDR output** (`r4_hdr_output`, Video > Basic "HDR" and "HDR Brightness"): the frame is drawn in SDR as before and
  converted to HDR10 at Present, so GAMMA's look stays (`r4_hdr_paper_white`, `r4_hdr_peak`, `r4_hdr_expand`). Steam
  Deck OLED: HDR on in Display settings and `DXVK_HDR=1` in the launch options.
- Variable rate shading for sun and lamp lighting where the image is flat (`r4_vrs`, on, tier 2 GPUs; `r4_vrs_debug`).
- Shaders a level uses compiled ahead on worker threads after a settings change (`r4_shader_precompile`).

## Frame time (same image, on by default)
- Hi-Z occlusion culling on the GPU (`r4_hiz_cull`): hidden trees, statics and dynamic objects skipped.
- Sun shadow caster culling (`r4_sun_caster_cull`), the three sun cascades culled in parallel (`r4_sun_parallel_cull`),
  far sun cascades kept between frames (`r4_sun_cascade_cache`, with their trees: `r4_sun_cascade_cache_trees`, Deck
  -4.5 ms GPU), shadow map draw order (`r4_smap_order`).
- Tree instancing (`r4_tree_instancing`): trees of the same model share one draw in the view and the shadow maps.
- Lamp shadow maps kept between frames for lamps that nothing moves through (`r4_light_smap_keep`), lamp queries in
  parallel (`r4_lights_parallel_query`).
- Constant buffers uploaded and bound only when changed (`r4_cbuffer_skip_unchanged`, `r4_constants_per_frame`).
- Post-process ping-pong instead of copies (`r4_post_pingpong`), water SSR skipped with no water in view.
- Main view visibility in parallel (`r4_main_parallel_cull`, on with 12 or more hardware threads), bones on all cores
  (`mt_calc_bones_parallel`).
- Waitable swap chain for lower input latency (`r4_swap_wait`, `r4_frame_latency`).
- Shader warm-up at the start (`r4_shader_warmup`) with a progress window (`r4_shader_progress`), and a counter while
  shaders compile during the game.
- Shader fixes (`r4_shader_patches`): early-outs and cheaper sampling in GAMMA's post-process shaders, same image.
- Rain drops born at most a 16th of them a frame (each casts a ray): a weather set at once no longer casts ~2500 rays
  in one frame (20-25 ms on the Deck); the drops fill in over ~16 frames, and a gradual weather change stays under it.

Steam Deck, Cordon, FSR balanced: GPU ~22.6 -> ~19.7 ms per frame, main thread ~20.5 -> ~18.5 ms, draw calls
~5,600 -> ~4,400.

## Lua garbage collector
Modded Exes steps the Lua GC on a worker thread while the frame renders. `lua_parallel_gc_paced`:
- 0: as Modded Exes.
- 1 (default): the worker steps it only when Lua's own pace calls for it, and allocations in scripts on the main
  thread no longer step it (unless the heap outgrows that pace, as during loads). The finalizers (`__gc`, game code)
  run on the main thread at the start of the level's frame, up to `lua_gc_finalize_us` (1000) a frame.
- 2: also the userdata to be finalized are separated in steps after the atomic phase instead of in it (30-50 ms with
  GAMMA). On in the Steam Deck preset; being tested before it becomes the default.

`lua_gc_pause` sets Lua's GC pause (0 leaves it to scripts). Village flight on the Deck, 0 -> 2: 49 -> 57 fps, 1% low
17 -> 29 fps, longest frame ~190 -> 83 ms, stutters 126 -> 20.

Fewer objects to collect: `device()`, `alife()`, `game_graph()`, `get_console()` and `get_hud()` return the same
script object every call instead of a new one (each a userdata the GC finalizes), and a game object has one script
object while it exists (callbacks, `level.object_by_id` and the like made a new one each time). With GAMMA, a 3-minute
flight: 9.0 -> 3.3-3.5 million engine objects made by scripts (`device()` 1.7 million, `alife()` 1 million, game
objects 2.4 million). Two lookups of the same object now give the same script object (`==` true).

## A-Life
A-Life updates its objects (squads and smart terrains, 20 at a time with GAMMA) on a worker thread while the frame
renders, and the frame waits for it at its end; squads looking for a new target cost 3-10 ms each in Lua, so an update
with several of them held a frame for 30-80 ms. `alife_scheduled_budget_ms` (5, 0 off): an
update starts no object after that long and the ones left are done in the next frames, before the next update's own,
so every object is still updated as often. Village flight on the Deck, 0 -> 5: A-Life's longest 37 -> 21-24 ms, frames
over 33 ms 34 -> 17-18.

Characters coming online: their models are prefetched on a thread (Modded Exes' `spawn_antifreeze`), and then every
prefetched object was created in one frame: a squad and its gear, up to 330 ms on the Deck. `spawn_frame_budget_ms` (4, 0
off): after that long in a frame the next characters wait for the following frames, each
with its items in the same frame (at least one a frame). Deck, a second flight in a session (squads coming online):
the level's longest frame step 330 -> 36 ms.

Spawning a character picks its specific character from all character descriptions, each one looked up in the shared
data map at every spawn: the descriptions' data is now looked up once (the same picks), a quarter of the time a squad
took to spawn.

A server object by its id (`alife():object`, `alife_object`, `get_object_squad`, thousands of times a frame with GAMMA's
scripts) comes from an array indexed by the id instead of a search of the object map. `get_object_squad` given a game
object goes to its server object at once (its `group_id` look-up only missed, the slowest kind on an engine object).

## Memory
- `texture_lod` really applied (D3DX11 ignored the mip skip), scope reticles always at full detail.
- Weather sky textures loaded on demand (`r4_sky_on_demand`).
- Texture streaming (`r4_tex_stream`): world textures load small with the level and in full in the background on
  first use; unused ones drop back after 60 s. UI, fonts, LUTs, sky and reticles are never streamed.

Texture memory at `texture_lod 0`: 6.25 GB -> 2.5-2.8 GB.

## Loading
- Collision model cache (`appdata\cform_cache`), script bytecode cache, ini hash index.
- Native string and ini helpers for `_g.script`, Lua functions looked up by key instead of walking `_G`.
- File index on worker threads, script folders scanned once, 30 precache frames instead of 60.
- The next level's files read in the background near its level changer (`level_preload`).
- Level changes behind a black fade instead of the loading screen (`fade_transitions`), saves load without "press any
  key" (`keypress_on_start 0`).

Level transition Cordon -> Swamp on the Deck: ~26 s -> ~16.5 s.

## Controls
- Gamepad (XInput): buttons bound like keys (`gp_bind`), a STALKER 2-like default layout (LT + R3 the alternative
  aim, the weapon's other sight), analog movement, a radial menu for weapons and devices, a cursor in menus, prompts
  with the pad's button icons (`gp_glyphs`, 0 names them), rumble. Options > Modded Exes > Controls > Gamepad, with a
  buttons overview (the controller and what each button does, pressed, held and with LT).
- DualSense and DualShock 4 over USB and Bluetooth: gyro aiming (`gp_gyro` 0 off, 1 while aiming, 2 always,
  `gp_gyro_sens`), PlayStation button names, adaptive trigger resistance on firearms.

## Patches for GAMMA's mods
Without changing what the mods do, and without editing their files:
- In the engine's gamedata (`zzzz_nextray_perf.script`, `zzzz_nextray_load.script`): Dynamic Anomalies' per-frame
  closures through one dispatcher, Useful Idiots' emission cover cache built over the first frames, 3DSS marker shaders
  normalized once per weapon after a load and not for weapons other characters hold (that loaded their HUD models,
  never drawn: 7-45 ms each on the Deck), story item lookups through the engine, `simulation_objects.get_server_entity`
  through an index instead of a scan of the ~1000 simulation objects (each squad update: ~2 s of a 3-minute flight on
  the Deck's main thread), Guards Spawner's check of every enemy check (it went through every guard with lookups and a
  second pass over the guards; now only the guard that is one of the two characters: ~10% of the NPCs' update time
  once guards are out). Each applies only when the functions
  it relies on are the known ones (their bytecode or source); otherwise it logs their fingerprints and stays off.
  `appdata\nextray_perf_off` turns them off.
- Shaders, in the engine's gamedata (`shaders/nextray/r3`): cheaper paths in Screen Space Shaders' terrain, AO, fog
  scattering, SSR, debanding, far sun fade, SSS extra lights and tree benders, each behind a bit of `r4_shader_patches`.
  A patched file is read in place of the mod's only while that one is the exact version it was made from (its CRC32 on
  the patch's first line; files that depend on each other only together), otherwise the mod's file as it is, with a
  log line. Deck, village flight: GPU per frame ~1.5 ms less, the terrain 2.3 -> 1.1 ms (layers of weight 0: no
  height, base or bump fetched). The `neXt-Ray performance patches` MO2 mod of earlier builds held them: no longer
  needed (its copies are recognised and left out).

## Fixes
- Freeze with `mt_calc_bones_parallel 1` in crowded places (foot IK of two models deadlocked).
- Deadlock at a save's load between texture loading threads and GPU memory accounting.
- Recursive `file_list_open_ex` dropped loose files in subfolders.
- Wine/macOS: ImGui windows scaled to the back buffer when the window's client area has another size.

## Tools
- Performance overlay (`perf_overlay`): FPS over time windows, CPU/GPU/engine times, a CPU- or GPU-bound hint, VRAM and RAM.
- Profilers: `stat_gpu_passes` (`r4_gpu_passes_split 1`: static geometry and shadow maps per pixel shader),
  `stat_cpu_passes`, `stat_next_load`, `stat_script_load`, `stat_gpu_memory`,
  `stat_tex_stream`, `stat_hiz`, `stat_sun_casters`, `stat_frame_pacing <s>` (every frame to a CSV in `logs/`; while it
  records, slow work to the log: a frame callback or scheduled object over 20 ms, an object spawned over 10 ms, a HUD
  model made over 5 ms),
  `stat_lua_sample <s>` (A-Life's, NPCs' and the actor's updates sampled: time by object, Lua function and C++ function,
  `logs\lua_sample_<time>.txt`),
  `stat_luabind_objects 1/0` (the engine objects scripts make, by class).
- Mod cost overlay (`perf_overlay_mods`): Lua time per frame and script load time for each MO2 mod.
- Crash reports: `appdata\logs\crash_<date>.txt` with the probable cause (script and its mod, or the engine function),
  where the scripts run, the engine and Lua stacks and the log's end.
- Hang watchdog (`hang_watchdog`, 60 s, three times that while loading, 0 off): a game whose main thread stops making
  progress writes a crash report (where it is stuck) and every thread's stack (`logs\hang_<time>.txt`), says so and
  closes.
- Camera flights for benchmarks: `flight_record <name>`, `flight_play <name> [hour]` (the weather changes every 15 s,
  no HUD), results with fps, 1% low and stutters per weather in `logs/bench_*.txt`.
- `stat_shader_stalls`, `stat_dx12`.
- Remote console (`-remote_console`), see [DEVELOPMENT.md](DEVELOPMENT.md#testing-remote-console).
- `mouse_filter 1`: averages the mouse over two frames for uneven mouse input (macOS); off by default.
