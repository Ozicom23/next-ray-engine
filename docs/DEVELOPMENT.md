# Development

Building, testing and the rules for changes. Back to the [README](../README.md).

## Building

The build is the CI's (`.github/workflows/dev-build.yml`): configuration `DX11-AVX` builds both `AnomalyDX11AVX.exe`
and `AnomalyDX12AVX.exe`. A plain `msbuild` silently leaves out DLSS and FSR; they need:
1. the NVIDIA DLSS SDK (tag in `dev-build.yml`) in `src/3rd party/NVIDIA-DLSS`,
2. the FSR shaders: `src/3rd party/FidelityFX-FSR3/compile_shaders.ps1`,
3. the frame generation shaders: `compile_shaders_dx12.ps1 -Compiler <FidelityFX_SC.exe>` (FidelityFX SDK v1.1.4),
4. `msbuild /m /p:Configuration=DX11-AVX src/engine-vs2022.sln`.

Every push to `master` runs the dev build in GitHub Actions (~38 min) and uploads the package and the PDBs.

### Local build on a Windows VM: `tools/vm-build/build.sh`

From macOS or Linux, the working tree is built on a Windows machine over SSH, incrementally (a one-line change
relinks in ~5 min, mostly LTCG):

```sh
tools/vm-build/build.sh                 # build the tree as it is (uncommitted files included)
tools/vm-build/build.sh --install /path/to/gamma   # build, then install into that game folder (local or user@host:/path); repeatable
tools/vm-build/build.sh --deck          # build, then install on the Steam Deck over SSH
tools/vm-build/build.sh --package --fetch   # the CI artifact layout, copied to _build/vm/package
tools/vm-build/build.sh --rebuild       # full rebuild
```

- The tree is pushed as a snapshot commit; your index and branches are untouched.
- `build.ps1` does the CI steps on the VM: it fetches the DLSS SDK and recompiles the FSR shaders only when they changed.
- Install: the exes and `nvngx_dlss.dll` to `anomaly/bin` (the replaced ones go to `backup-nextray-exes-prev/`),
  `gamedata/` to `anomaly/gamedata` (the old patch mod in `gamma/mods` is emptied). Close the game first.
- Environment: `NEXTRAY_VM` (default `windows@192.168.64.4`), `NEXTRAY_VM_DIR` (`C:/nr`), `NEXTRAY_DECK`.
- `--quick` builds without LTCG in its own checkout on the VM (a one-file change in under a minute): for test builds only, never benchmarks or releases.

VM setup (once): Windows 11 (ARM64 works, it builds x64 with the cross compilers), OpenSSH server, Git, VS 2022 Build
Tools with the C++ workload, FidelityFX SDK v1.1.4 in `C:\nr-deps\ffx-sdk`, and an SSH key for the Mac.

## Testing: remote console

Start the game with `-dbg -remote_console` (MO2: the executable's arguments). The engine then reads console commands
from `appdata/remote_cmd.txt` and writes its state twice a second to `appdata/remote_status.txt`
(`state=menu|level|loading|idle`, `fps`, `frame`, `queue`, `waiting`, `last`). Write the command file to a temporary name
and rename it, one command per line; the game deletes it when it has taken the commands.

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

Useful in tests: `screenshot`, `dbg_freeze 1` (time stops, rendering goes on), `vid_restart`, `run_string <lua>`
(about 240 characters at most), and the profilers in [FEATURES.md](FEATURES.md#tools). A frozen game writes a hang
dump (`logs/hang_*.txt`).

## Rules for changes
- No visual or behaviour change from GAMMA without an option; new optimizations must give the same image.
- Mods are not edited. Script fixes for them go into the engine's gamedata and apply only to the mod's known version
  (fingerprints); shader fixes go into `gamedata/shaders/nextray/<path>`, the whole patched file with the original's
  CRC32 on its first line (`CRC32 <zlib crc32 of the mod's file>`, `group <name>` for files that only work together).
  A changed shader A/Bs in separate game sessions on D3D12 (`vid_restart` keeps the compiled shaders).
- Don't name variables `far` / `near` (Windows macros).
