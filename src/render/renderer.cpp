#include "render/renderer.h"
#include "render/screen_geometry.h"

#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gt {
namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Vertex {
    float x, y, z;
    float r, g, b, a;
    float u, v;
};

const char* kShaderSource = R"(
cbuffer Transform : register(b0) {
    float4x4 g_view_proj;
};

struct VSInput {
    float3 pos : POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
};

struct PSInput {
    float4 pos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
};

Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

PSInput vs_main(VSInput input) {
    PSInput output;
    output.pos = mul(float4(input.pos, 1.0f), g_view_proj);
    output.color = input.color;
    output.uv = input.uv;
    return output;
}

float4 ps_main(PSInput input) : SV_TARGET {
    return g_texture.Sample(g_sampler, input.uv) * input.color;
}
)";

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring output(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        output.data(), count);
    return output;
}

bool create_solid_texture(ID3D11Device* device, uint32_t bgra,
                          ComPtr<ID3D11ShaderResourceView>& view) {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 1;
    description.Height = 1;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = &bgra;
    data.SysMemPitch = sizeof(bgra);
    ComPtr<ID3D11Texture2D> texture;
    return SUCCEEDED(device->CreateTexture2D(&description, &data, &texture)) &&
           SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &view));
}

bool create_label_texture(ID3D11Device* device, const ScreenLayout& screen,
                          ComPtr<ID3D11ShaderResourceView>& view) {
    constexpr int width = 512;
    constexpr int height = 288;
    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bitmap_info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    if (bitmap == nullptr || dc == nullptr || pixels == nullptr) {
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (dc != nullptr) DeleteDC(dc);
        return false;
    }
    const HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    const BYTE red = static_cast<BYTE>(screen.color[0] * 255.0f);
    const BYTE green = static_cast<BYTE>(screen.color[1] * 255.0f);
    const BYTE blue = static_cast<BYTE>(screen.color[2] * 255.0f);
    RECT bounds{0, 0, width, height};
    HBRUSH background = CreateSolidBrush(RGB(red, green, blue));
    FillRect(dc, &bounds, background);
    DeleteObject(background);

    HPEN border = CreatePen(PS_SOLID, 8, RGB(245, 247, 255));
    const HGDIOBJ old_pen = SelectObject(dc, border);
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, 4, 4, width - 4, height - 4);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(border);

    HFONT font = CreateFontW(72, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FF_DONTCARE, L"Segoe UI");
    const HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    std::wstring label = widen(screen.id);
    DrawTextW(dc, label.data(), static_cast<int>(label.size()), &bounds,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, old_font);
    DeleteObject(font);

    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = pixels;
    data.SysMemPitch = width * 4;
    ComPtr<ID3D11Texture2D> texture;
    const bool ok = SUCCEEDED(device->CreateTexture2D(&description, &data, &texture)) &&
                    SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &view));
    SelectObject(dc, old_bitmap);
    DeleteDC(dc);
    DeleteObject(bitmap);
    return ok;
}

}  // namespace

bool Renderer::init(HWND hwnd, uint32_t width, uint32_t height, std::string& error) {
    width_ = width;
    height_ = height;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
                                   D3D11_SDK_VERSION, &device_, &level, &context_);
    if (FAILED(hr)) {
        error = "D3D11CreateDevice failed";
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    hr = device_.As(&dxgi_device);
    if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    ComPtr<IDXGISwapChain1> swapchain1;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &desc, nullptr, nullptr, &swapchain1);
    }
    if (FAILED(hr)) {
        error = "CreateSwapChainForHwnd failed";
        return false;
    }
    hr = swapchain1.As(&swapchain_);
    if (FAILED(hr)) {
        error = "IDXGISwapChain2 not available";
        return false;
    }
    swapchain_->SetMaximumFrameLatency(1);
    frame_latency_waitable_ = swapchain_->GetFrameLatencyWaitableObject();

    ComPtr<ID3D11Texture2D> backbuffer;
    hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    if (FAILED(hr) || FAILED(device_->CreateRenderTargetView(backbuffer.Get(), nullptr, &render_target_))) {
        error = "render target creation failed";
        return false;
    }

    D3D11_TEXTURE2D_DESC depth_desc{};
    depth_desc.Width = width;
    depth_desc.Height = height;
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device_->CreateTexture2D(&depth_desc, nullptr, &depth_texture_)) ||
        FAILED(device_->CreateDepthStencilView(depth_texture_.Get(), nullptr, &depth_view_))) {
        error = "depth buffer creation failed";
        return false;
    }

    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> ps_blob;
    ComPtr<ID3DBlob> errors;
    hr = D3DCompile(kShaderSource, std::strlen(kShaderSource), "scene.hlsl", nullptr, nullptr, "vs_main",
                    "vs_5_0", 0, 0, &vs_blob, &errors);
    if (FAILED(hr)) {
        error = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "vertex shader compile failed";
        return false;
    }
    hr = D3DCompile(kShaderSource, std::strlen(kShaderSource), "scene.hlsl", nullptr, nullptr, "ps_main",
                    "ps_5_0", 0, 0, &ps_blob, &errors);
    if (FAILED(hr)) {
        error = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "pixel shader compile failed";
        return false;
    }
    if (FAILED(device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr,
                                           &vertex_shader_)) ||
        FAILED(device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr,
                                          &pixel_shader_))) {
        error = "shader creation failed";
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(device_->CreateInputLayout(elements, 3, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                          &input_layout_))) {
        error = "input layout creation failed";
        return false;
    }

    D3D11_BUFFER_DESC vb_desc{};
    constexpr UINT kMaximumVertices = 84 + 8 * 6 + 6;
    vb_desc.ByteWidth = kMaximumVertices * sizeof(Vertex);
    vb_desc.Usage = D3D11_USAGE_DYNAMIC;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&vb_desc, nullptr, &vertex_buffer_))) {
        error = "vertex buffer creation failed";
        return false;
    }

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sampler_desc, &sampler_)) ||
        !create_solid_texture(device_.Get(), 0xFFFFFFFFu, white_texture_)) {
        error = "texture resources creation failed";
        return false;
    }

    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = 64;
    cb_desc.Usage = D3D11_USAGE_DEFAULT;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device_->CreateBuffer(&cb_desc, nullptr, &constant_buffer_))) {
        error = "constant buffer creation failed";
        return false;
    }

    D3D11_RASTERIZER_DESC rast{};
    rast.FillMode = D3D11_FILL_SOLID;
    rast.CullMode = D3D11_CULL_NONE;
    rast.DepthClipEnable = TRUE;
    if (FAILED(device_->CreateRasterizerState(&rast, &rasterizer_))) {
        error = "rasterizer state creation failed";
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC depth_desc_state{};
    depth_desc_state.DepthEnable = TRUE;
    depth_desc_state.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_desc_state.DepthFunc = D3D11_COMPARISON_LESS;
    depth_desc_state.StencilEnable = FALSE;
    if (FAILED(device_->CreateDepthStencilState(&depth_desc_state, &depth_state_))) {
        error = "depth stencil state creation failed";
        return false;
    }
    depth_desc_state.DepthEnable = FALSE;
    depth_desc_state.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(device_->CreateDepthStencilState(&depth_desc_state, &depth_disabled_state_))) {
        error = "depth-disabled stencil state creation failed";
        return false;
    }
    return set_layout(default_layout(), error);
}

bool Renderer::set_layout(const Layout& layout, std::string& error) {
    if (!device_ || !context_ || !vertex_buffer_) {
        error = "renderer is not initialized";
        return false;
    }
    if (!validate_layout(layout, error)) {
        return false;
    }

    std::vector<Vertex> vertices;
    vertices.reserve(84 + layout.screens.size() * 6 + 6);
    for (int i = -10; i <= 10; ++i) {
        const float t = static_cast<float>(i) * 0.5f;
        vertices.push_back(Vertex{t, 0.0f, -5.0f, 0.22f, 0.22f, 0.26f, 1.0f, 0.5f, 0.5f});
        vertices.push_back(Vertex{t, 0.0f, 5.0f, 0.22f, 0.22f, 0.26f, 1.0f, 0.5f, 0.5f});
        vertices.push_back(Vertex{-5.0f, 0.0f, t, 0.22f, 0.22f, 0.26f, 1.0f, 0.5f, 0.5f});
        vertices.push_back(Vertex{5.0f, 0.0f, t, 0.22f, 0.22f, 0.26f, 1.0f, 0.5f, 0.5f});
    }
    const UINT new_line_vertex_count = static_cast<UINT>(vertices.size());

    std::vector<ScreenDraw> new_draws;
    new_draws.reserve(layout.screens.size());
    for (const ScreenLayout& screen : layout.screens) {
        ScreenDraw draw;
        draw.vertex_start = static_cast<UINT>(vertices.size());
        if (!create_label_texture(device_.Get(), screen, draw.texture)) {
            error = "failed to create label texture for screen '" + screen.id + "'";
            return false;
        }
        const auto geometry = make_screen_quad(screen);
        for (const ScreenGeometryVertex& point : geometry) {
            vertices.push_back(Vertex{point.x, point.y, point.z, 1.0f, 1.0f, 1.0f, 1.0f,
                                      point.u, point.v});
        }
        new_draws.push_back(std::move(draw));
    }

    const UINT new_crosshair_start = static_cast<UINT>(vertices.size());
    constexpr float s = 0.010f;
    constexpr float z = 0.5f;
    vertices.push_back(Vertex{-s, -s, z, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.5f});
    vertices.push_back(Vertex{s, -s, z, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.5f});
    vertices.push_back(Vertex{s, s, z, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.5f});
    vertices.push_back(Vertex{-s, -s, z, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.5f});
    vertices.push_back(Vertex{s, s, z, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.5f});
    vertices.push_back(Vertex{-s, s, z, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.5f});

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(vertex_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        error = "failed to update screen geometry";
        return false;
    }
    std::memcpy(mapped.pData, vertices.data(), vertices.size() * sizeof(Vertex));
    context_->Unmap(vertex_buffer_.Get(), 0);

    line_vertex_count_ = new_line_vertex_count;
    crosshair_vertex_start_ = new_crosshair_start;
    screen_draws_ = std::move(new_draws);
    return true;
}

void Renderer::shutdown() {
    if (context_) {
        context_->ClearState();
    }
    frame_latency_waitable_ = nullptr;
    swapchain_.Reset();
    render_target_.Reset();
    depth_view_.Reset();
    depth_texture_.Reset();
    vertex_shader_.Reset();
    pixel_shader_.Reset();
    input_layout_.Reset();
    vertex_buffer_.Reset();
    constant_buffer_.Reset();
    rasterizer_.Reset();
    depth_state_.Reset();
    depth_disabled_state_.Reset();
    sampler_.Reset();
    white_texture_.Reset();
    screen_draws_.clear();
    context_.Reset();
    device_.Reset();
}

void Renderer::render(const Quat& head, float fov_horizontal_deg, float time_s) {
    (void)time_s;
    const float clear_color[4] = {0.02f, 0.02f, 0.03f, 1.0f};
    context_->ClearRenderTargetView(render_target_.Get(), clear_color);
    context_->ClearDepthStencilView(depth_view_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    ID3D11RenderTargetView* targets[] = {render_target_.Get()};
    context_->OMSetRenderTargets(1, targets, depth_view_.Get());
    context_->OMSetDepthStencilState(depth_state_.Get(), 0);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizer_.Get());

    const UINT stride = sizeof(Vertex);
    const UINT offset = 0;
    ID3D11Buffer* buffers[] = {vertex_buffer_.Get()};
    context_->IASetVertexBuffers(0, 1, buffers, &stride, &offset);
    context_->IASetInputLayout(input_layout_.Get());
    context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
    ID3D11SamplerState* samplers[] = {sampler_.Get()};
    context_->PSSetSamplers(0, 1, samplers);
    ID3D11Buffer* cb_buffers[] = {constant_buffer_.Get()};
    context_->VSSetConstantBuffers(0, 1, cb_buffers);
    context_->PSSetConstantBuffers(0, 1, cb_buffers);

    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const float fov_y =
        2.0f * std::atan(std::tan(fov_horizontal_deg * kPi / 360.0f) / aspect);
    const DirectX::XMMATRIX projection =
        DirectX::XMMatrixPerspectiveFovLH(fov_y, aspect, 0.05f, 200.0f);

    const DirectX::XMMATRIX view = camera_view_matrix(head, signs_);

    DirectX::XMFLOAT4X4 matrix;
    DirectX::XMStoreFloat4x4(&matrix, DirectX::XMMatrixTranspose(view * projection));
    context_->UpdateSubresource(constant_buffer_.Get(), 0, nullptr, &matrix, 0, 0);

    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ID3D11ShaderResourceView* white[] = {white_texture_.Get()};
    context_->PSSetShaderResources(0, 1, white);
    context_->Draw(line_vertex_count_, 0);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (const ScreenDraw& screen : screen_draws_) {
        ID3D11ShaderResourceView* texture[] = {screen.texture.Get()};
        context_->PSSetShaderResources(0, 1, texture);
        context_->Draw(6, screen.vertex_start);
    }

    DirectX::XMStoreFloat4x4(&matrix, DirectX::XMMatrixTranspose(projection));
    context_->UpdateSubresource(constant_buffer_.Get(), 0, nullptr, &matrix, 0, 0);
    context_->OMSetDepthStencilState(depth_disabled_state_.Get(), 0);
    context_->PSSetShaderResources(0, 1, white);
    context_->Draw(6, crosshair_vertex_start_);
}

void Renderer::wait_for_frame() {
    if (frame_latency_waitable_ != nullptr) {
        WaitForSingleObjectEx(frame_latency_waitable_, 1000, FALSE);
    }
}

bool Renderer::present() {
    if (!swapchain_) {
        return false;
    }
    return SUCCEEDED(swapchain_->Present(1, 0));
}

}  // namespace gt
