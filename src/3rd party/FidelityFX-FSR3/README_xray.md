# FidelityFX FSR 3.1 upscaler (vendored)

Subset of AMD FidelityFX SDK **v1.1.4** (https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), MIT license (see `LICENSE.txt`),
at FSR **3.1.5**: `include/FidelityFX/gpu/fsr1/ffx_fsr1.h` from FSR SDK v2.3.0 (the RCAS fix for negative output, the only
upscaler change of 3.1.5; the other shader differences of v2.3.0 are register macro renames):

- `include/FidelityFX/host`: host API headers used by the FSR 3.1 upscaler
- `include/FidelityFX/gpu`: shader headers (core, fsr1, fsr3upscaler, spd)
- `src/components/fsr3upscaler`, `src/shared`: host-side upscaler implementation
- `shaders`: compute pass entry points (from `sdk/src/backends/dx12/shaders/fsr3upscaler`)

AMD ships only DX12/Vulkan backends. The engine provides its own D3D11 backend, and the passes are compiled for `cs_5_0` with fxc by `compile_shaders.ps1` (CI runs it before building).

## Local patches

Search for `XRAY-DX11` to find them when updating the SDK.

| File | Change |
|---|---|
| `include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_accumulate.h` | zero-initialize the `Accumulate()` locals; fxc rejects passing partially initialized structs as `inout` |
| `shaders/ffx_fsr3upscaler_luma_pyramid_pass.hlsl` | renumber UAV slots so the used ones fit D3D11.0's 8 compute UAV slots (u0-u7) |
