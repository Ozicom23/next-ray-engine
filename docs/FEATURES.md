# Features

What neXt-Ray adds to Modded Exes, with the console commands and their defaults. Back to the [README](../README.md).

## Upscaling
DLSS 4, FSR 3.1 and XeSS (XeSS on DX12 only). Video > Advanced.

| Command | Default | |
|---|---|---|
| `r4_upscaler` | `off` | `off`, `dlss`, `fsr`, `xess` |
| `r4_upscaler_quality` | `quality` | `ultra_performance`, `performance`, `balanced`, `quality`, `native` |
| `r4_upscaler_scale` | 0 | custom render scale, 0 = the quality mode's |
| `r4_dlss_preset` | `default` | DLSS model preset |
| `r4_fsr_fp16` | 1 | FSR with 16-bit math |
| `r4_upscaler_late_overlays` | 1 | gas mask, night vision and scopes drawn after the upscaler |
| `r4_upscaler_scope_late` | 1 | 3DSS scope lenses drawn after the upscaler |
| `r4_upscaler_pda_late` | 1 | PDA screen drawn after the upscaler |
| `r4_upscaler_jitter` | 2 | camera jitter: 0 off, 1 on, 2 off while aiming through a lens |

## DirectX 12
`AnomalyDX12AVX.exe`. DX11 (`AnomalyDX11AVX.exe`) stays for older PCs.

| Command | Default | |
|---|---|---|
| `r4_framegen` | `off` | frame generation: `off`, `fsr`, `xess` (needs an upscaler) |
| `r4_framegen_async` | 1 | FSR frame generation on the async compute queue |
| `r4_vrs` | 1 | variable rate shading where the image is flat |
| `r4_shader_precompile` | 1 | the level's shaders compiled ahead after a settings change |
| `r__frame_time_smoothing` | 1 | smoother frame times |

## HDR
HDR10 output that keeps GAMMA's look. Video > Basic.

| Command | Default | |
|---|---|---|
| `r4_hdr_output` | 0 | HDR on |
| `r4_hdr_paper_white` | 200 | UI and SDR white brightness (nits) |
| `r4_hdr_peak` | 1000 | display peak brightness (nits) |
| `r4_hdr_scene` | 1 | bright lights, fire and flashes go above SDR white (DX12) |
| `r4_hdr_highlights` | 1 | how far they go, 0..1 |
| `r4_hdr_sky` | 3 | sky brightness, times SDR |

Steam Deck OLED: turn HDR on in the Deck's display settings and add `DXVK_HDR=1` to the launch options.

## Performance
All on by default, same image.

| Command | Default | |
|---|---|---|
| `r4_hiz_cull` | 1 | GPU occlusion culling |
| `r4_tree_instancing` | 1 | trees of the same model drawn together |
| `r4_sun_caster_cull` | 1 | sun shadow caster culling |
| `r4_sun_parallel_cull` | 1 | sun cascades culled in parallel |
| `r4_main_parallel_cull` | 1 on 12+ threads | main view culled in parallel |
| `r4_sun_cascade_cache` | 2 | far sun shadows reused between frames (0 off, 1 farthest cascade, 2 both far ones) |
| `r4_sun_cascade_cache_trees` | 2 | tree shadows kept in them too |
| `r4_light_smap_keep` | 1 | lamp shadows reused while nothing moves near the lamp |
| `r4_lights_parallel_query` | 1 | lamps found in parallel |
| `r4_smap_order` | 2 | shadow map draw order |
| `r4_cbuffer_skip_unchanged` | 1 | constant buffers uploaded only when changed |
| `r4_constants_per_frame` | 1 | per-frame constants set once per frame |
| `r4_post_pingpong` | 1 | no copies between post-process passes |
| `r4_swap_wait` | 1 | waitable swap chain, lower input latency |
| `r4_frame_latency` | 2 | frames queued ahead |
| `r4_shader_warmup` | 1 | shaders compiled at the start |
| `r4_shader_progress` | 1 | shader compile progress on screen |
| `r4_shader_patches` | `0xDFF` | cheaper versions of GAMMA's shaders, one bit each |
| `mt_calc_bones_parallel` | 1 | animations on all cores |

## Lua and A-Life

| Command | Default | |
|---|---|---|
| `lua_parallel_gc_paced` | 1 | Lua garbage collector: 0 as Modded Exes, 1 paced in the background, 2 also finalizers split (testing) |
| `lua_gc_finalize_us` | 1000 | time per frame for finalizers (µs) |
| `lua_gc_pause` | 0 | Lua GC pause, 0 = the scripts' choice |
| `alife_scheduled_budget_ms` | 5 | A-Life time per frame; the rest moves to the next frames (0 off) |
| `spawn_frame_budget_ms` | 4 | time per frame for NPCs coming online (0 off) |

Also: faster LuaJIT (fewer failed traces), faster calls between scripts and the engine.

## Memory

| Command | Default | |
|---|---|---|
| `r4_tex_stream` | 1 | texture streaming |
| `r4_sky_on_demand` | 1 | weather sky textures loaded when used |

`texture_lod` now really lowers texture quality (D3DX11 ignored it).

## Loading

| Command | Default | |
|---|---|---|
| `fade_transitions` | 1 | level changes behind a fade to black, no loading screen |
| `level_preload` | 1 | the next level read ahead near its level changer |

Also: collision and script caches, a faster file index, faster ini and script loading.

## Steam Deck preset
Options > Video > Advanced: one switch that lowers the settings whose loss is hard to see on the Deck's 800p screen
(smaller shadow maps, ambient occlusion and reflections at half resolution, fewer shadow and indirect light samples,
8x anisotropic filtering). Turning it off restores your previous values.

## Gamepad (in progress)
XInput pads, DualSense and DualShock 4: a STALKER 2-like layout, radial menus, button prompts, gyro aiming, rumble and
adaptive triggers. Options > Modded Exes > Controls > Gamepad.

| Command | Default | |
|---|---|---|
| `gp_enabled` | 1 | gamepad on |
| `gp_bind`, `gp_unbind` | | bind a button like a key |
| `gp_glyphs` | 1 | button icons in prompts |
| `gp_gyro` | 1 | gyro aiming: 0 off, 1 while aiming, 2 always |
| `gp_gyro_sens` | 1.5 | gyro sensitivity |

## GAMMA's mods
Cheaper versions of some of GAMMA's heaviest scripts (Dynamic Anomalies, Useful Idiots, 3DSS, Guards Spawner and
others) and of Screen Space Shaders' shaders. The mods' files are not touched, and each patch only applies to the exact
version of the mod it was made for. A file `appdata\nextray_perf_off` turns the script patches off, `r4_shader_patches 0`
the shader ones.

## Tools

| Command | |
|---|---|
| `perf_overlay` | performance overlay: `off`, `compact`, `graph`, `detailed` |
| `perf_overlay_mods` | Lua time per MO2 mod |
| `hang_watchdog` | 60: a frozen game writes a report and closes after that many seconds (0 off) |
| `flight_record`, `flight_play` | record and replay camera flights as benchmarks |
| `stat_frame_pacing`, `stat_lua_sample`, `stat_cpu_passes`, `stat_gpu_passes`, `stat_tex_stream`, ... | profilers |
| `mouse_filter` | 0: smooths uneven mouse input (macOS) |

Crash reports go to `appdata\logs\crash_<date>.txt` and name the script and mod at fault.
