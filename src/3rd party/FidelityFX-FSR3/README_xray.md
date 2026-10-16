# FidelityFX FSR 3.1 upscaler (vendored)

Subset of AMD FidelityFX SDK **v1.1.4** (https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), MIT license (see `LICENSE.txt`),
at FSR **3.1.5**: `include/FidelityFX/gpu/fsr1/ffx_fsr1.h` from FSR SDK v2.3.0 (the RCAS fix for negative output, the only
upscaler change of 3.1.5; the other shader differences of v2.3.0 are register macro renames):

- `include/FidelityFX/host`: host API headers used by the FSR 3.1 upscaler
- `include/FidelityFX/gpu`: shader headers (core, fsr1, fsr3upscaler, spd)
- `src/components/fsr3upscaler`, `src/shared`: host-side upscaler implementation
- `shaders`: compute pass entry points (from `sdk/src/backends/dx12/shaders/fsr3upscaler`)

AMD ships only DX12/Vulkan backends. The engine provides its own D3D11 backend, and the passes are compiled for `cs_5_0` with fxc by `compile_shaders.ps1` (CI runs it before building).

## FSR 3.1 frame generation (DX12 exe only)

From the same SDK v1.1.4 (MIT), used by `src/Layers/xrRenderPC_R4/r4_framegen.cpp` in interpolation-only mode:

- `include/FidelityFX/host`: `ffx_frameinterpolation.h`, `ffx_opticalflow.h`, `ffx_fsr3.h` (types the swap chain uses),
  `ffx_breadcrumbs.h`, `backends/dx12` (`ffx_dx12.h`, `d3dx12.h`)
- `include/FidelityFX/gpu/frameinterpolation`, `include/FidelityFX/gpu/opticalflow`: shader headers
- `src/components/frameinterpolation`, `src/components/opticalflow`: host-side implementations
- `src/backends/dx12/ffx_dx12.cpp`: AMD's D3D12 backend; `src/backends/dx12/FrameInterpolationSwapchain`: the frame
  interpolation swap chain; `src/backends/dx12/shaders/{frameinterpolation,opticalflow}`: pass entry points
- `src/backends/shared`: shader blob lookup (built with `FFX_FI`, `FFX_OF` only), `src/shared/ffx_breadcrumbs_list.*`,
  `src/libs/antilag2/ffx_antilag2_dx12.h`

The passes need SM 6 (optical flow uses wave operations), so they are compiled to DXIL with AMD's shader compiler
(`FidelityFX_SC.exe` from the SDK's binary store, fetched by CI) by `compile_shaders_dx12.ps1` into `generated_dx12`,
the permutation headers the blob accessors include. The engine's own and GAMMA's shaders stay on fxc.

## Local patches

Search for `XRAY-DX11` and `XRAY-DX12` to find them when updating the SDK.

| File | Change |
|---|---|
| `include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_accumulate.h` | zero-initialize the `Accumulate()` locals; fxc rejects passing partially initialized structs as `inout` |
| `shaders/ffx_fsr3upscaler_luma_pyramid_pass.hlsl` | renumber UAV slots so the used ones fit D3D11.0's 8 compute UAV slots (u0-u7) |
| `src/backends/dx12/ffx_dx12.cpp` | `XRAY-DX12`: PIX captures off (`pix3.h` and its runtime are not vendored) |
