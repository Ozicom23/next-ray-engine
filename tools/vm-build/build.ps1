# Builds the engine on the Windows VM the way the dev build CI does (.github/workflows/dev-build.yml), started by
# tools/vm-build/build.sh from the Mac after it has checked out the snapshot to build.
# Usage: powershell -File build.ps1 [-Conf DX11-AVX] [-Package] [-Rebuild]
#   -Package: also packs gamedata and stages package/ (bin, db/mods, mods) like the CI artifact
#   -Rebuild: msbuild /t:Rebuild instead of an incremental build
# The DLSS and XeSS SDKs and the FSR shaders are fetched/compiled only when missing or when their inputs changed.

param(
    [string]$Conf = "DX11-AVX",
    [switch]$Package,
    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$deps = "$root\..\nr-deps"
Set-Location $root
New-Item -ItemType Directory -Force -Path $deps | Out-Null

function Step($name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan }
function Check($what) { if ($LASTEXITCODE -ne 0) { throw "$what failed ($LASTEXITCODE)" } }

# Same SDK tag as the CI
$tag = (Select-String -Path ".github\workflows\dev-build.yml" -Pattern "DLSS_SDK_TAG:\s*(\S+)").Matches[0].Groups[1].Value

Step "NVIDIA DLSS SDK $tag"
$dlss = "src\3rd party\NVIDIA-DLSS"
$have = if (Test-Path "$dlss\.git") { git -C $dlss tag --points-at HEAD } else { "" }
if ($have -ne $tag) {
    if (Test-Path $dlss) { Remove-Item -Recurse -Force $dlss }
    git -c advice.detachedHead=false clone -q --depth 1 --branch $tag --filter=blob:none --sparse https://github.com/NVIDIA/DLSS.git $dlss; Check "DLSS SDK clone"
    git -C $dlss sparse-checkout set --no-cone /include/ /LICENSE.txt /lib/Windows_x86_64/x64/nvsdk_ngx_d.lib /lib/Windows_x86_64/rel/nvngx_dlss.dll /lib/Windows_x86_64/dev/nvngx_dlss.dll; Check "DLSS SDK checkout"
} else { Write-Host "up to date" }

$xtag = (Select-String -Path ".github\workflows\dev-build.yml" -Pattern "XESS_SDK_TAG:\s*(\S+)").Matches[0].Groups[1].Value
Step "Intel XeSS SDK $xtag"
$xess = "src\3rd party\Intel-XeSS"
$have = if (Test-Path "$xess\.git") { git -C $xess tag --points-at HEAD } else { "" }
if ($have -notcontains $xtag) {
    if (Test-Path $xess) { Remove-Item -Recurse -Force $xess }
    git -c advice.detachedHead=false clone -q --depth 1 --branch $xtag --filter=blob:none --sparse https://github.com/intel/xess.git $xess; Check "XeSS SDK clone"
    git -C $xess sparse-checkout set --no-cone /inc/ /lib/ /LICENSE.txt /bin/libxess.dll /bin/libxess_fg.dll /bin/libxell.dll; Check "XeSS SDK checkout"
} else { Write-Host "up to date" }

# The FSR shaders are recompiled when the FSR folder's tree (in the built snapshot) changed
$fsr = "src\3rd party\FidelityFX-FSR3"
$fsrTree = git rev-parse "HEAD:src/3rd party/FidelityFX-FSR3"; Check "rev-parse"

Step "FSR shaders"
if ((Get-Content "$fsr\generated\.stamp" -ErrorAction SilentlyContinue) -ne $fsrTree) {
    powershell -NoProfile -ExecutionPolicy Bypass -File "$fsr\compile_shaders.ps1"; Check "FSR shaders"
    Set-Content "$fsr\generated\.stamp" $fsrTree
} else { Write-Host "up to date" }

Step "FSR frame generation shaders (DX12 exe)"
if ((Get-Content "$fsr\generated_dx12\.stamp" -ErrorAction SilentlyContinue) -ne $fsrTree) {
    $sdk = "$deps\ffx-sdk"
    if (!(Test-Path "$sdk\sdk\tools\binary_store\FidelityFX_SC.exe")) {
        if (Test-Path $sdk) { Remove-Item -Recurse -Force $sdk }
        git -c advice.detachedHead=false clone -q --depth 1 --branch v1.1.4 --filter=blob:none --sparse https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git $sdk; Check "FidelityFX SDK clone"
        git -C $sdk sparse-checkout set --no-cone /sdk/tools/binary_store/; Check "FidelityFX SDK checkout"
    }
    powershell -NoProfile -ExecutionPolicy Bypass -File "$fsr\compile_shaders_dx12.ps1" -Compiler "$sdk\sdk\tools\binary_store\FidelityFX_SC.exe"; Check "FSR DX12 shaders"
    Set-Content "$fsr\generated_dx12\.stamp" $fsrTree
} else { Write-Host "up to date" }

Step "Build $Conf"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
$msbuild = @("$vs\MSBuild\Current\Bin\arm64\MSBuild.exe", "$vs\MSBuild\Current\Bin\amd64\MSBuild.exe", "$vs\MSBuild\Current\Bin\MSBuild.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $msbuild) { throw "MSBuild not found (Visual Studio 2022 Build Tools)" }
$target = if ($Rebuild) { "Rebuild" } else { "Build" }
& $msbuild /m /nologo /v:m /t:$target /p:Configuration=$Conf src\engine-vs2022.sln; Check "msbuild"

Step "Crash report symbols"
# Each exe writes its function names from its PDB next to itself (<exe>.sym, read by the crash reports)
# (the DLLs it links with are found in sdk\binaries and the XeSS SDK)
$env:PATH = "$root\sdk\binaries;$root\$xess\bin;$env:PATH"
Get-ChildItem "_build\_game\bin_dbg\*.exe" | ForEach-Object {
    $sym = [IO.Path]::ChangeExtension($_.FullName, ".sym")
    if (!(Test-Path $sym) -or (Get-Item $sym).LastWriteTime -lt $_.LastWriteTime) {
        $p = Start-Process -FilePath $_.FullName -ArgumentList "-dump_symbols `"$sym`"" -Wait -PassThru -NoNewWindow
        if ($p.ExitCode -ne 0) { Write-Host "no symbols for $($_.Name) ($($p.ExitCode))" -ForegroundColor Yellow }
        else { Write-Host "$($_.Name): $([math]::Round((Get-Item $sym).Length / 1MB, 1)) MB" }
    } else { Write-Host "$($_.Name): up to date" }
}

if ($Package) {
    Step "Pack gamedata"
    $db = "00_modded_exes_gamedata.db0"
    Remove-Item -Recurse -Force "compressor\gamedata", "compressor\gamedata.db0", "compressor\$db" -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path "compressor\gamedata" | Out-Null
    Copy-Item -Path "gamedata\*" -Destination "compressor\gamedata" -Recurse -Force
    Push-Location compressor
    Start-Process -FilePath ".\! compress_gamedata.cmd" -Wait -NoNewWindow
    Rename-Item -Path gamedata.db0 -NewName $db
    Remove-Item -Recurse -Force gamedata
    Pop-Location

    Step "Stage package"
    Remove-Item -Recurse -Force package, package_pdb -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path "package\bin\dlss_dev", "package\db\mods", "package\mods", "package_pdb\bin" | Out-Null
    Copy-Item "_build\_game\bin_dbg\*.exe" "package\bin"
    Copy-Item "_build\_game\bin_dbg\*.sym" "package\bin" -ErrorAction SilentlyContinue
    Copy-Item "_build\_game\bin_dbg\*.pdb" "package_pdb\bin"
    Move-Item "compressor\$db" "package\db\mods"
    Copy-Item "mods\neXt-Ray performance patches" "package\mods" -Recurse
    Copy-Item "$dlss\lib\Windows_x86_64\rel\nvngx_dlss.dll" "package\bin"
    Copy-Item "$dlss\LICENSE.txt" "package\bin\nvngx_dlss_LICENSE.txt"
    Copy-Item "$dlss\lib\Windows_x86_64\dev\nvngx_dlss.dll" "package\bin\dlss_dev"
    Copy-Item "$xess\bin\libxess.dll", "$xess\bin\libxess_fg.dll", "$xess\bin\libxell.dll" "package\bin"
    Copy-Item "$xess\LICENSE.txt" "package\bin\libxess_LICENSE.txt"
}

Step "Done"
Get-ChildItem "_build\_game\bin_dbg\*.exe" | Format-Table Name, Length, LastWriteTime -AutoSize
