// Headless WARP test of the actual production ImGui renderer, no desktop input.
#include <Windows.h>
#include <d3d11.h>
#include <cstdio>
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const D3D_FEATURE_LEVEL levels[] { D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL created {};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 1, D3D11_SDK_VERSION, &device, &created, &context)))
        return 1;
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(64, 64);
    io.DeltaTime = 1.f / 60.f;
    if (!ImGui_ImplDX11_Init(device, context))
        return 2;

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = desc.Height = 64;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* target = nullptr;
    ID3D11RenderTargetView* view = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &target)) ||
        FAILED(device->CreateRenderTargetView(target, nullptr, &view)))
        return 3;
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
        return 4;
    for (int iteration = 0; iteration != 3; ++iteration)
    {
        // Also cover device-object invalidation/recreation.
        if (!ImGui_ImplDX11_CreateDeviceObjects())
            return 5;
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(8, 8), ImVec2(56, 56), IM_COL32(255, 0, 0, 255));
        ImGui::Render();
        const float clear[4] {};
        context->OMSetRenderTargets(1, &view, nullptr);
        context->ClearRenderTargetView(view, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        context->CopyResource(staging, target);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
            return 6;
        const auto* pixel = static_cast<const unsigned char*>(mapped.pData) + 32 * mapped.RowPitch + 32 * 4;
        const bool red = pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255;
        context->Unmap(staging, 0);
        if (!red)
            return 7;
        ImGui_ImplDX11_InvalidateDeviceObjects();
    }
    staging->Release();
    view->Release();
    target->Release();
    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    context->ClearState();
    context->Release();
    device->Release();
    std::puts("PASS: embedded DXBC, WARP feature level 10.0, ImGui pixel readback, 3 recreations; no window.");
    return 0;
}
