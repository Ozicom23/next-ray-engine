# Development

Building, testing and the rules for changes. Back to the [README](../README.md).

## Renderers

DX12 (`AnomalyDX12AVX.exe`, `src/Layers/xrRenderDX12`) is the main renderer: new features land there first, and
performance is measured there. DX11 (`AnomalyDX11AVX.exe`) is kept working for older PCs. Both exes share the R4
renderer code in `src/Layers/xrRenderPC_R4`; code for one API only sits behind `USE_DX12`.

Some features are DX12 only: XeSS, frame generation, variable rate shading, HDR scene highlights.

## Building

Visual Studio 2022, configuration `DX11-AVX` of `src/engine-vs2022.sln`. It builds both exes.

A plain build silently leaves out the upscalers. Before it, as in the CI (`.github/workflows/dev-build.yml`):
1. the NVIDIA DLSS SDK (tag `DLSS_SDK_TAG` in `dev-build.yml`) into `src/3rd party/NVIDIA-DLSS`,
2. the Intel XeSS SDK (tag `XESS_SDK_TAG`) into `src/3rd party/Intel-XeSS`,
3. the FSR shaders: `src/3rd party/FidelityFX-FSR3/compile_shaders.ps1`,
4. the frame generation shaders: `compile_shaders_dx12.ps1 -Compiler <FidelityFX_SC.exe>` (FidelityFX SDK v1.1.4),
5. `msbuild /m /p:Configuration=DX11-AVX src/engine-vs2022.sln`.

The game needs `nvngx_dlss.dll` and the XeSS DLLs (`libxess.dll`, `libxess_fg.dll`, `libxell.dll`) next to the exes.

Every push to `master` runs the dev build in GitHub Actions and uploads the package and the debug symbols.

## Branches

- `master`: stable, what releases are made from.
- `multiplayer`: the multiplayer work in progress.

## Testing

Test on DX12 first, then check that DX11 still works. A shader change is compared in separate game sessions on DX12
(`vid_restart` keeps the compiled shaders).

### Remote console

Start the game with `-dbg -remote_console` (MO2: the executable's arguments). The engine then reads console commands
from `appdata/remote_cmd.txt` and writes its state twice a second to `appdata/remote_status.txt`
(`state=menu|level|loading|idle`, `fps`, `frame`, `queue`, `waiting`, `last`). Write the command file under a
temporary name and rename it, one command per line; the game deletes it when it has taken the commands.

Commands only for the remote console:
- `rc_wait <s>`: wait before the next commands,
- `rc_wait_level <s>`: wait until a level is loaded and running,
- `rc_clear`: drop the queued commands,
- `rc_silent_level_changers 1`: level changers travel without their dialog.

`tools/remote_console.sh` (Steam Deck, or macOS with `--root /path/to/gamma` first, the folder with anomaly/ and gamma/):

```sh
tools/remote_console.sh launch              # start the game through Steam, wait for the main menu
tools/remote_console.sh load test           # load a save (no spaces in its name), wait until it runs
tools/remote_console.sh send "perf_overlay detailed" "stat_frame_pacing 20"
tools/remote_console.sh run commands.txt    # queue a file of commands
tools/remote_console.sh idle                # wait until the queue is done
tools/remote_console.sh transition x y z    # travel through a level changer, print the load phase times
tools/remote_console.sh status | log 50 | quit
```

Useful in tests: `screenshot`, `dbg_freeze 1` (time stops, rendering goes on), `vid_restart`, `run_string <lua>`, and
the profilers in [FEATURES.md](FEATURES.md#tools). A frozen game writes a hang dump (`logs/hang_*.txt`).

## Rules for changes
- No visual or gameplay change from GAMMA without an option; optimizations must give the same image.
- Mods are not edited. Script fixes for them go into the engine's gamedata and apply only to the mod's known version
  (fingerprints); shader fixes go into `gamedata/shaders/nextray/<path>`, the whole patched file with the original's
  CRC32 on its first line (`CRC32 <zlib crc32 of the mod's file>`, `group <name>` for files that only work together).
- Don't name variables `far` / `near` (Windows macros).
