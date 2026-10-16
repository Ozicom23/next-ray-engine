# Compiles the FSR 3.1 frame generation passes (frame interpolation, optical flow) for AMD's D3D12 backend into the
# permutation headers its blob accessors include, as the SDK's CMake does (FidelityFX SDK v1.1.4,
# sdk/include/FidelityFX/gpu/CMakeCompileShaders.txt): DXIL, four variants per pass (wave32 cs_6_2, wave64 cs_6_6,
# each with and without 16-bit types). The DX12 exe only; the shader compiler comes from the SDK's binary store.
# Usage: powershell -File compile_shaders_dx12.ps1 -Compiler <FidelityFX_SC.exe> [-OutDir <dir>]

param(
    [Parameter(Mandatory = $true)][string]$Compiler,
    [string]$OutDir = "$PSScriptRoot\generated_dx12"
)

$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$Compiler = (Resolve-Path $Compiler).Path
$gpu = (Resolve-Path "$PSScriptRoot\include\FidelityFX\gpu").Path

$apiArgs = @("-E", "CS", "-Wno-for-redefinition", "-Wno-ambig-lit-shift", "-DFFX_HLSL=1")
$effects = @(
    @{
        Dir = "frameinterpolation"
        Base = @(
            "-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
            "-DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
            "-DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
            "-DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
            "-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
            "-reflection", "-deps=gcc", "-DFFX_GPU=1")
        Permutations = @(
            "-DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS={0,1}",
            "-DFFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS={0,1}",
            "-DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH={0,1}")
    },
    @{
        Dir = "opticalflow"
        Base = @("-reflection", "-deps=gcc", "-DFFX_GPU=1")
        Permutations = @("-DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT={0,1}")
    }
)

$variants = @(
    @{ Suffix = "";             Args = @("-DFFX_HALF=0", "-DFFX_HLSL_SM=62", "-T", "cs_6_2") },
    @{ Suffix = "_wave64";      Args = @("-DFFX_HALF=0", "-DFFX_PREFER_WAVE64=[WaveSize(64)]", "-DFFX_HLSL_SM=66", "-T", "cs_6_6") },
    @{ Suffix = "_16bit";       Args = @("-DFFX_HALF=1", "-enable-16bit-types", "-DFFX_HLSL_SM=62", "-T", "cs_6_2") },
    @{ Suffix = "_wave64_16bit"; Args = @("-DFFX_HALF=1", "-enable-16bit-types", "-DFFX_PREFER_WAVE64=[WaveSize(64)]", "-DFFX_HLSL_SM=66", "-T", "cs_6_6") }
)

$failed = 0
$count = 0
foreach ($effect in $effects) {
    $includes = @("-I", $gpu, "-I", (Join-Path $gpu $effect.Dir))
    foreach ($shader in Get-ChildItem "$PSScriptRoot\src\backends\dx12\shaders\$($effect.Dir)\*.hlsl") {
        foreach ($v in $variants) {
            $scArgs = $effect.Base + $apiArgs + $effect.Permutations + @("-name=$($shader.BaseName)$($v.Suffix)") + $v.Args + $includes + @("-output=$OutDir", $shader.FullName)
            & $Compiler @scArgs | Out-Host
            $header = Join-Path $OutDir "$($shader.BaseName)$($v.Suffix)_permutations.h"
            if ($LASTEXITCODE -ne 0 -or -not (Test-Path $header)) {
                Write-Host "FAILED: $($shader.Name) $($v.Suffix)"
                $failed++
            } else {
                $count++
            }
        }
    }
}

Write-Host "$count permutation headers in $OutDir"
if ($failed) { throw "$failed frame generation shader variants failed to compile" }
