#include "stdafx.h"
#include "dxImGuiRender.h"

#include <imgui.h>

#if defined(USE_DX12)
#include <backends/imgui_impl_dx12.h>
#elif defined(USE_DX11)
#include <backends/imgui_impl_dx11.h>
#elif defined(USE_DX10)
#include <backends/imgui_impl_dx10.h>
#else
#include <backends/imgui_impl_dx9.h>
#endif

#if defined(USE_DX12)
namespace
{
// The font's SRV: a static slot of the context's shader-visible heap, kept until the device goes away (the backend
// frees it on every InvalidateDeviceObjects but reuses the handle in CreateDeviceObjects)
D3D12_CPU_DESCRIPTOR_HANDLE s_font_cpu = {};
D3D12_GPU_DESCRIPTOR_HANDLE s_font_gpu = {};
DXGI_FORMAT s_rtv_format = DXGI_FORMAT_UNKNOWN; // the backend's pipeline is built for it

void imgui_srv_alloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    if (!s_font_cpu.ptr && !HW.pContext->AllocStaticDescriptor(&s_font_cpu, &s_font_gpu))
    {
        s_font_cpu.ptr = 0;
        s_font_gpu.ptr = 0;
        Msg("! ImGui: no descriptor for the font texture");
    }
    *cpu = s_font_cpu;
    *gpu = s_font_gpu;
}

void imgui_srv_free(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {}

void imgui_init_dx12()
{
    // The swap chain's buffers, the target ImGui draws into
    dx12::Texture2D* back_buffer = HW.pDevice->CurrentBackBuffer();
    s_rtv_format = back_buffer ? back_buffer->m_format : HW.m_ChainDesc.Format;

    ImGui_ImplDX12_InitInfo info;
    info.Device = HW.pDevice->Native();
    info.CommandQueue = HW.pDevice->Queue(); // font upload
    info.NumFramesInFlight = 3; // dx12::kFrames
    info.RTVFormat = s_rtv_format;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = HW.pContext->ShaderVisibleHeap();
    info.SrvDescriptorAllocFn = imgui_srv_alloc;
    info.SrvDescriptorFreeFn = imgui_srv_free;
    ImGui_ImplDX12_Init(&info);
}
}
#endif

void dxImGuiRender::Copy(IImGuiRender& _in)
{
    *this = *fast_dynamic_cast<dxImGuiRender*>(&_in);
}

void dxImGuiRender::SetState(ImDrawData* data)
{
#if defined(USE_DX11) || defined(USE_DX10)
    D3D_VIEWPORT VP = { 0, 0, data->DisplaySize.x, data->DisplaySize.y, 0, 1.f };
#if defined(USE_DX11)
    HW.pContext->RSSetViewports(1, &VP);
#else
    HW.pDevice->RSSetViewports(1, &VP);
#endif
#else
    D3DVIEWPORT9 VP = { 0, 0, data->DisplaySize.x, data->DisplaySize.y, 0, 1.f };
    HW.pDevice->SetViewport(&VP);
#endif

    // Setup shader and vertex buffers
    /*unsigned int stride = sizeof(ImDrawVert);
    unsigned int offset = 0;
    ctx->IASetInputLayout(bd->pInputLayout);
    ctx->IASetVertexBuffers(0, 1, &bd->pVB, &stride, &offset);
    ctx->IASetIndexBuffer(bd->pIB, sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(bd->pVertexShader, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &bd->pVertexConstantBuffer);
    ctx->PSSetShader(bd->pPixelShader, nullptr, 0);
    ctx->PSSetSamplers(0, 1, &bd->pFontSampler);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0); // In theory we should backup and restore this as well.. very infrequently used..
    ctx->DSSetShader(nullptr, nullptr, 0); // In theory we should backup and restore this as well.. very infrequently used..
    ctx->CSSetShader(nullptr, nullptr, 0); // In theory we should backup and restore this as well.. very infrequently used..

    // Setup blend state
    const float blend_factor[4] = { 0.f, 0.f, 0.f, 0.f };
    ctx->OMSetBlendState(bd->pBlendState, blend_factor, 0xffffffff);
    ctx->OMSetDepthStencilState(bd->pDepthStencilState, 0);
    ctx->RSSetState(bd->pRasterizerState);*/
}

void dxImGuiRender::Frame()
{
#if defined(USE_DX12)
    ImGui_ImplDX12_NewFrame();
#elif defined(USE_DX11)
    ImGui_ImplDX11_NewFrame();
#elif defined(USE_DX10)
    ImGui_ImplDX10_NewFrame();
#else
    ImGui_ImplDX9_NewFrame();
#endif
}

// ImGui works in the window's client area (its mouse input is in client coordinates) but draws into the back buffer.
// When the two differ (Wine on macOS: a 2304x1296 client area for a 3456x2234 back buffer) ImGui's windows landed in
// the top-left part of the screen and too small; scaled up to the back buffer here. Same size: nothing to do
static void FitToBackBuffer(ImDrawData* data)
{
    if (!data || data->DisplaySize.x <= 0.f || data->DisplaySize.y <= 0.f || !Device.dwWidth || !Device.dwHeight)
        return;
    const ImVec2 scale(float(Device.dwWidth) / data->DisplaySize.x, float(Device.dwHeight) / data->DisplaySize.y);
    if (fabsf(scale.x - 1.f) < 0.001f && fabsf(scale.y - 1.f) < 0.001f)
        return;
    const ImVec2 origin = data->DisplayPos;
    for (int i = 0; i < data->CmdLists.Size; ++i)
        for (ImDrawVert& v : data->CmdLists[i]->VtxBuffer)
        {
            v.pos.x = origin.x + (v.pos.x - origin.x) * scale.x;
            v.pos.y = origin.y + (v.pos.y - origin.y) * scale.y;
        }
    for (int i = 0; i < data->CmdLists.Size; ++i)
        for (ImDrawCmd& cmd : data->CmdLists[i]->CmdBuffer)
            cmd.ClipRect = ImVec4(origin.x + (cmd.ClipRect.x - origin.x) * scale.x, origin.y + (cmd.ClipRect.y - origin.y) * scale.y,
                                  origin.x + (cmd.ClipRect.z - origin.x) * scale.x, origin.y + (cmd.ClipRect.w - origin.y) * scale.y);
    data->DisplaySize = ImVec2(float(Device.dwWidth), float(Device.dwHeight));
}

void dxImGuiRender::Render(ImDrawData* data)
{
    FitToBackBuffer(data);
#if defined(USE_DX12)
    if (!data || data->DisplaySize.x <= 0.f || data->DisplaySize.y <= 0.f || !data->TotalVtxCount)
        return;

    // Into the bound render target like the D3D11 backend, when the backend's pipeline fits it; the back buffer
    // otherwise (resolved to the current swap chain buffer where the commands are recorded)
    dx12::Context* ctx = HW.pContext;
    dx12::RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv && (rtv->m_back_buffer || !rtv->m_res || rtv->m_format != s_rtv_format))
        _RELEASE(rtv);

    // The draw lists are rebuilt next frame while the copy is recorded (on the context's recording thread)
    ImDrawData* copy = IM_NEW(ImDrawData)();
    *copy = *data;
    for (int i = 0; i < copy->CmdLists.Size; ++i)
        copy->CmdLists[i] = data->CmdLists[i]->CloneOutput();

    ctx->ExecuteNative([rtv, copy](dx12::Context::NativeRecorder& r) {
        dx12::Resource* target = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
        if (rtv)
        {
            target = rtv->m_res;
            handle = rtv->m_cpu;
        }
        else
            target = r.BackBuffer(&handle);
        if (target && handle.ptr)
        {
            r.Transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
            ID3D12GraphicsCommandList* list = r.List();
            list->OMSetRenderTargets(1, &handle, FALSE, nullptr);
            ID3D12DescriptorHeap* heap = HW.pContext->ShaderVisibleHeap();
            list->SetDescriptorHeaps(1, &heap);
            // Sets its viewport, scissors, pipeline and root signature
            ImGui_ImplDX12_RenderDrawData(copy, list);
        }
        for (int i = 0; i < copy->CmdLists.Size; ++i)
            IM_DELETE(copy->CmdLists[i]);
        IM_DELETE(copy);
        if (rtv)
            rtv->Release();
    });
#elif defined(USE_DX11)
    ImGui_ImplDX11_RenderDrawData(data);
#elif defined(USE_DX10)
    ImGui_ImplDX10_RenderDrawData(data);
#else
    ImGui_ImplDX9_RenderDrawData(data);
#endif
}

void dxImGuiRender::OnDeviceCreate(ImGuiContext* context)
{
    ImGui::SetAllocatorFunctions(
        [](size_t size, void* /*user_data*/)
        {
            return xr_malloc(size);
        },
        [](void* ptr, void* /*user_data*/)
        {
            xr_free(ptr);
        }
    );
    ImGui::SetCurrentContext(context);

#if defined(USE_DX12)
    imgui_init_dx12();
#elif defined(USE_DX11)
    ImGui_ImplDX11_Init(HW.pDevice, HW.pContext);
#elif defined(USE_DX10)
    ImGui_ImplDX10_Init(HW.pDevice);
#else
    ImGui_ImplDX9_Init(HW.pDevice);
#endif
}
void dxImGuiRender::OnDeviceDestroy()
{
#if defined(USE_DX12)
    // The backend releases its buffers, font and pipeline right away
    HW.pDevice->WaitIdle();
    ImGui_ImplDX12_Shutdown();
    if (s_font_cpu.ptr)
        HW.pContext->FreeStaticDescriptor(s_font_cpu);
    s_font_cpu.ptr = 0;
    s_font_gpu.ptr = 0;
#elif defined(USE_DX11)
    ImGui_ImplDX11_Shutdown();
#elif defined(USE_DX10)
    ImGui_ImplDX10_Shutdown();
#else
    ImGui_ImplDX9_Shutdown();
#endif
}

void dxImGuiRender::OnDeviceResetBegin()
{
#if defined(USE_DX12)
    // Shut down and initialized again at the end: the back buffer format may change with the swap chain
    HW.pDevice->WaitIdle();
    ImGui_ImplDX12_Shutdown();
#elif defined(USE_DX11)
    ImGui_ImplDX11_InvalidateDeviceObjects();
#elif defined(USE_DX10)
    ImGui_ImplDX10_InvalidateDeviceObjects();
#else
    ImGui_ImplDX9_InvalidateDeviceObjects();
#endif
}

void dxImGuiRender::OnDeviceResetEnd()
{
#if defined(USE_DX12)
    imgui_init_dx12();
#elif defined(USE_DX11)
    ImGui_ImplDX11_CreateDeviceObjects();
#elif defined(USE_DX10)
    ImGui_ImplDX10_CreateDeviceObjects();
#else
    ImGui_ImplDX9_CreateDeviceObjects();
#endif
}