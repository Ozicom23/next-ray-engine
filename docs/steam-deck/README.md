# GAMMA and neXt-Ray on the Steam Deck

Installing GAMMA with [stalker-gamma-cli](https://github.com/FaithBeam/stalker-gamma-cli), running it from Steam, and
installing neXt-Ray.

Everything below is done in Desktop Mode, in Konsole. GAMMA needs about 150 GB of free space.

## Before you start

From Discover, install **Protontricks**.

## 1. Install GAMMA

```sh
mkdir -p ~/Games/gamma && cd ~/Games/gamma
wget https://github.com/FaithBeam/stalker-gamma-cli/releases/latest/download/stalker-gamma+linux.x64.AppImage
chmod +x ./stalker-gamma+linux.x64.AppImage
./stalker-gamma+linux.x64.AppImage config create --anomaly anomaly --gamma gamma --cache cache
./stalker-gamma+linux.x64.AppImage full-install
```

The install takes a few hours. If it stops partway, run `full-install --resume`.

## 2. Add it to Steam

1. Steam > Games > Add a Non-Steam Game to My Library > Browse: `~/Games/gamma/gamma/ModOrganizer.exe` (set the
   file type to All Files).
2. The new entry's Properties > Compatibility: force Proton Experimental. In Properties > Shortcut, rename it from
   `ModOrganizer` to something you'll recognize in your library, e.g. `Stalker GAMMA`
3. Start it once, so Steam creates its Proton prefix; Mod Organizer opens. Close it.

## 3. Protontricks

Find the entry's id, then install the libraries GAMMA needs into its prefix:

```sh
flatpak run com.github.Matoking.protontricks -l
flatpak run com.github.Matoking.protontricks <id> d3dcompiler_43 d3dcompiler_47 d3dx10 d3dx11_43 d3dx9 vcrun2022
```

Installers pop up along the way: accept each license and click through them until Protontricks finishes.

## 4. Install neXt-Ray

```sh
cd ~/Games/gamma
mkdir -p backup-before-nextray/bin
mv anomaly/bin/AnomalyDX11AVX.exe backup-before-nextray/bin/
mv anomaly/gamedata backup-before-nextray/
wget https://github.com/Ozicom23/next-ray-engine/releases/download/v0.2.1/neXt-Ray-0.2.1.zip
unzip -o neXt-Ray-0.2.1.zip -d anomaly
```

The `gamedata` folder is moved into the backup because it holds GAMMA's engine scripts and shaders as loose files,
and loose files take priority over neXt-Ray's.

## 5. Start the DX12 exe from Steam

1. Start the Steam entry: Mod Organizer opens. Open the executables list (the gears icon) and select
   `Anomaly (DX11-AVX)`: copy its Binary and Start in. Then Add, and fill in:
   - Title: `Anomaly (DX12-AVX)`
   - Binary: the copied path with `AnomalyDX11AVX.exe` changed to `AnomalyDX12AVX.exe`
   - Start in: the copied path, as it is (the `anomaly/bin` folder)
   - check **Force load libraries** and **Use application's icon for desktop shortcuts**

   Apply, then close Mod Organizer.
2. The Steam entry's Properties > Launch Options. Steam Deck LCD:
   ```
   "moshortcut://:Anomaly (DX12-AVX)"
   ```
   Steam Deck OLED (with HDR):
   ```
   DXVK_HDR=1 %command% "moshortcut://:Anomaly (DX12-AVX)"
   ```
   It now starts the game directly, also in Game Mode. Without the launch options it opens Mod Organizer again.

In the game, turn on the Steam Deck preset: Options > Video > Advanced.

## After a GAMMA update

```sh
cd ~/Games/gamma
./stalker-gamma+linux.x64.AppImage update apply --preserve-user-settings --preserve-mcm-settings
rm -rf anomaly/gamedata
unzip -o neXt-Ray-0.2.1.zip -d anomaly
```

An update can put GAMMA's engine and its `gamedata` folder back. Check that `Anomaly (DX12-AVX)` is still in Mod
Organizer's list (step 5).

## Uninstall

```sh
cd ~/Games/gamma
rm -f anomaly/bin/AnomalyDX12AVX.exe anomaly/db/mods/00_modded_exes_gamedata.db0
mv backup-before-nextray/bin/AnomalyDX11AVX.exe anomaly/bin/
mv backup-before-nextray/gamedata anomaly/
```

Then set the launch options to `DXVK_HDR=1 %command% "moshortcut://:Anomaly (DX11-AVX)"`.
