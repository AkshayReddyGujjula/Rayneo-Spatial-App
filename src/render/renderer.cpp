#include "render/renderer.h"
#include "render/screen_geometry.h"

#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
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

cbuffer CursorInfo : register(b1) {
    float4 g_cursor_rect;
    uint g_cursor_mode;
    float3 g_cursor_padding;
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
Texture2D g_desktop : register(t1);
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

float4 ps_cursor(PSInput input) : SV_TARGET {
    float4 cursor = g_texture.Sample(g_sampler, input.uv);
    float2 desktop_uv = g_cursor_rect.xy + input.uv * g_cursor_rect.zw;
    float4 desktop = g_desktop.Sample(g_sampler, desktop_uv);
    if (g_cursor_mode == 0) {
        return cursor;
    }
    if (g_cursor_mode == 1) {
        if (cursor.a < 0.5f) {
            return float4(cursor.rgb, 1.0f);
        }
        uint3 background = (uint3)round(saturate(desktop.rgb) * 255.0f);
        uint3 mask = (uint3)round(saturate(cursor.rgb) * 255.0f);
        return float4((float3)(background ^ mask) / 255.0f, 1.0f);
    }
    float and_mask = cursor.r >= 0.5f ? 1.0f : 0.0f;
    float xor_mask = cursor.g >= 0.5f ? 1.0f : 0.0f;
    float3 composed = desktop.rgb * and_mask;
    composed = xor_mask > 0.5f ? 1.0f - composed : composed;
    return float4(composed, 1.0f);
}
)";

struct CursorConstants {
    float u;
    float v;
    float width;
    float height;
    uint32_t mode;
    float padding[3];
};

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

bool create_cursor_texture(ID3D11Device* device, const CursorUpdate& update,
                           ComPtr<ID3D11Texture2D>& texture,
                           ComPtr<ID3D11ShaderResourceView>& view, uint32_t& visible_height,
                           std::string& error) {
    if (update.shape_width == 0 || update.shape_height == 0 || update.shape_pitch == 0 ||
        update.shape_pixels == nullptr || update.shape_bytes == 0) {
        error = "cursor shape metadata is incomplete";
        return false;
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = update.shape_width;
    description.Height = update.shape_height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    std::vector<uint32_t> monochrome;

    if (update.mode == CursorShapeMode::Monochrome) {
        if ((update.shape_height % 2) != 0) {
            error = "monochrome cursor mask height must contain equal AND and XOR halves";
            return false;
        }
        visible_height = update.shape_height / 2;
        const size_t required = static_cast<size_t>(update.shape_pitch) * update.shape_height;
        if (update.shape_bytes < required) {
            error = "monochrome cursor buffer is shorter than its pitch and height";
            return false;
        }
        description.Height = visible_height;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        monochrome.resize(static_cast<size_t>(description.Width) * visible_height);
        for (uint32_t y = 0; y < visible_height; ++y) {
            const uint8_t* and_row = update.shape_pixels + static_cast<size_t>(y) * update.shape_pitch;
            const uint8_t* xor_row =
                update.shape_pixels + static_cast<size_t>(y + visible_height) * update.shape_pitch;
            for (uint32_t x = 0; x < description.Width; ++x) {
                const uint8_t bit = static_cast<uint8_t>(0x80u >> (x % 8));
                const uint8_t and_value = (and_row[x / 8] & bit) != 0 ? 255 : 0;
                const uint8_t xor_value = (xor_row[x / 8] & bit) != 0 ? 255 : 0;
                monochrome[static_cast<size_t>(y) * description.Width + x] =
                    0xFF000000u | (static_cast<uint32_t>(xor_value) << 8) | and_value;
            }
        }
        data.pSysMem = monochrome.data();
        data.SysMemPitch = description.Width * 4;
    } else {
        visible_height = update.shape_height;
        const size_t required = static_cast<size_t>(update.shape_pitch) * update.shape_height;
        if (update.shape_bytes < required || update.shape_pitch < update.shape_width * 4) {
            error = "colour cursor buffer is shorter than its BGRA pitch and height";
            return false;
        }
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        data.pSysMem = update.shape_pixels;
        data.SysMemPitch = update.shape_pitch;
    }

    texture.Reset();
    view.Reset();
    HRESULT result = device->CreateTexture2D(&description, &data, &texture);
    if (SUCCEEDED(result)) result = device->CreateShaderResourceView(texture.Get(), nullptr, &view);
    if (FAILED(result)) {
        error = "failed to create the desktop cursor texture";
        texture.Reset();
        return false;
    }
    return true;
}

Vertex interpolate_cursor_vertex(const std::array<ScreenGeometryVertex, 6>& quad, float screen_u,
                                 float screen_v, float cursor_u, float cursor_v) {
    const ScreenGeometryVertex& bottom_left = quad[0];
    const ScreenGeometryVertex& bottom_right = quad[1];
    const ScreenGeometryVertex& top_right = quad[2];
    const ScreenGeometryVertex& top_left = quad[5];
    const float top_weight = 1.0f - screen_v;
    const float bottom_weight = screen_v;
    const float left_weight = 1.0f - screen_u;
    const float right_weight = screen_u;
    const auto component = [&](float ScreenGeometryVertex::*member) {
        const float top = top_left.*member * left_weight + top_right.*member * right_weight;
        const float bottom =
            bottom_left.*member * left_weight + bottom_right.*member * right_weight;
        return (top * top_weight + bottom * bottom_weight) * 0.9995f;
    };
    return Vertex{component(&ScreenGeometryVertex::x), component(&ScreenGeometryVertex::y),
                  component(&ScreenGeometryVertex::z), 1.0f, 1.0f, 1.0f, 1.0f, cursor_u,
                  cursor_v};
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
    ComPtr<ID3DBlob> cursor_ps_blob;
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
    hr = D3DCompile(kShaderSource, std::strlen(kShaderSource), "scene.hlsl", nullptr, nullptr,
                    "ps_cursor", "ps_5_0", 0, 0, &cursor_ps_blob, &errors);
    if (FAILED(hr)) {
        error = errors ? static_cast<const char*>(errors->GetBufferPointer())
                       : "cursor shader compile failed";
        return false;
    }
    if (FAILED(device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr,
                                           &vertex_shader_)) ||
        FAILED(device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr,
                                          &pixel_shader_)) ||
        FAILED(device_->CreatePixelShader(cursor_ps_blob->GetBufferPointer(),
                                          cursor_ps_blob->GetBufferSize(), nullptr,
                                          &cursor_pixel_shader_))) {
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
    constexpr UINT kMaximumVertices = 8 * 6;
    vb_desc.ByteWidth = kMaximumVertices * sizeof(Vertex);
    vb_desc.Usage = D3D11_USAGE_DYNAMIC;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&vb_desc, nullptr, &vertex_buffer_))) {
        error = "vertex buffer creation failed";
        return false;
    }
    vb_desc.ByteWidth = 8 * 6 * sizeof(Vertex);
    if (FAILED(device_->CreateBuffer(&vb_desc, nullptr, &cursor_vertex_buffer_))) {
        error = "cursor vertex buffer creation failed";
        return false;
    }

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sampler_desc, &sampler_))) {
        error = "sampler creation failed";
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
    cb_desc.ByteWidth = sizeof(CursorConstants);
    if (FAILED(device_->CreateBuffer(&cb_desc, nullptr, &cursor_constant_buffer_))) {
        error = "cursor constant buffer creation failed";
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
    depth_desc_state.DepthEnable = TRUE;
    depth_desc_state.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth_desc_state.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(device_->CreateDepthStencilState(&depth_desc_state, &cursor_depth_state_))) {
        error = "cursor depth state creation failed";
        return false;
    }

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&blend, &cursor_blend_state_))) {
        error = "cursor blend state creation failed";
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
    vertices.reserve(layout.screens.size() * 6);

    std::vector<ScreenDraw> new_draws;
    new_draws.reserve(layout.screens.size());
    for (const ScreenLayout& screen : layout.screens) {
        ScreenDraw draw;
        draw.vertex_start = static_cast<UINT>(vertices.size());
        draw.layout = screen;
        if (!create_label_texture(device_.Get(), screen, draw.label_texture)) {
            error = "failed to create label texture for screen '" + screen.id + "'";
            return false;
        }
        const auto geometry = make_screen_quad(screen);
        draw.geometry = geometry;
        for (const ScreenGeometryVertex& point : geometry) {
            vertices.push_back(Vertex{point.x, point.y, point.z, 1.0f, 1.0f, 1.0f, 1.0f,
                                      point.u, point.v});
        }
        new_draws.push_back(std::move(draw));
    }


    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(vertex_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        error = "failed to update screen geometry";
        return false;
    }
    std::memcpy(mapped.pData, vertices.data(), vertices.size() * sizeof(Vertex));
    context_->Unmap(vertex_buffer_.Get(), 0);

    screen_draws_ = std::move(new_draws);
    return true;
}

bool Renderer::set_screen_texture(size_t screen_index, ID3D11ShaderResourceView* texture,
                                  std::string& error) {
    if (screen_index >= screen_draws_.size()) {
        error = "screen texture index is out of range";
        return false;
    }
    screen_draws_[screen_index].live_texture = texture;
    return true;
}

bool Renderer::update_screen_cursor(size_t screen_index, const CursorUpdate& update,
                                    std::string& error) {
    if (screen_index >= screen_draws_.size()) {
        error = "screen cursor index is out of range";
        return false;
    }
    ScreenDraw& screen = screen_draws_[screen_index];
    if (update.position_updated) {
        if (update.desktop_width == 0 || update.desktop_height == 0) {
            error = "cursor position update requires non-zero desktop dimensions";
            return false;
        }
        screen.cursor_visible = update.visible;
        screen.cursor_x = update.x;
        screen.cursor_y = update.y;
        screen.desktop_width = update.desktop_width;
        screen.desktop_height = update.desktop_height;
    }
    if (update.shape_updated) {
        uint32_t visible_height = 0;
        if (!create_cursor_texture(device_.Get(), update, screen.cursor_texture,
                                   screen.cursor_view, visible_height, error)) {
            return false;
        }
        screen.cursor_width = update.shape_width;
        screen.cursor_height = visible_height;
        screen.cursor_mode = update.mode;
    }
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
    cursor_pixel_shader_.Reset();
    input_layout_.Reset();
    vertex_buffer_.Reset();
    cursor_vertex_buffer_.Reset();
    constant_buffer_.Reset();
    cursor_constant_buffer_.Reset();
    rasterizer_.Reset();
    depth_state_.Reset();
    cursor_depth_state_.Reset();
    cursor_blend_state_.Reset();
    sampler_.Reset();
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

    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (const ScreenDraw& screen : screen_draws_) {
        ID3D11ShaderResourceView* texture[] = {
            screen.live_texture ? screen.live_texture.Get() : screen.label_texture.Get()};
        context_->PSSetShaderResources(0, 1, texture);
        context_->Draw(6, screen.vertex_start);
    }

    // validate_layout limits the workspace to eight screens. Keep the cursor
    // batch on the stack so a visible pointer does not allocate every frame.
    std::array<Vertex, 8 * 6> cursor_vertices;
    std::array<size_t, 8> cursor_screens;
    size_t cursor_count = 0;
    for (size_t index = 0; index < screen_draws_.size(); ++index) {
        const ScreenDraw& screen = screen_draws_[index];
        if (!screen.cursor_visible || !screen.cursor_view || !screen.live_texture ||
            screen.desktop_width == 0 || screen.desktop_height == 0 ||
            screen.cursor_width == 0 || screen.cursor_height == 0) {
            continue;
        }
        const float left = static_cast<float>(screen.cursor_x) / screen.desktop_width;
        const float top = static_cast<float>(screen.cursor_y) / screen.desktop_height;
        const float right = static_cast<float>(screen.cursor_x + static_cast<int>(screen.cursor_width)) /
                            screen.desktop_width;
        const float bottom =
            static_cast<float>(screen.cursor_y + static_cast<int>(screen.cursor_height)) /
            screen.desktop_height;
        const float clipped_left = std::clamp(left, 0.0f, 1.0f);
        const float clipped_top = std::clamp(top, 0.0f, 1.0f);
        const float clipped_right = std::clamp(right, 0.0f, 1.0f);
        const float clipped_bottom = std::clamp(bottom, 0.0f, 1.0f);
        if (clipped_left >= clipped_right || clipped_top >= clipped_bottom) continue;

        const float cursor_u0 = (clipped_left - left) / (right - left);
        const float cursor_v0 = (clipped_top - top) / (bottom - top);
        const float cursor_u1 = (clipped_right - left) / (right - left);
        const float cursor_v1 = (clipped_bottom - top) / (bottom - top);
        const Vertex top_left = interpolate_cursor_vertex(screen.geometry, clipped_left, clipped_top,
                                                           cursor_u0, cursor_v0);
        const Vertex top_right = interpolate_cursor_vertex(screen.geometry, clipped_right, clipped_top,
                                                            cursor_u1, cursor_v0);
        const Vertex bottom_right = interpolate_cursor_vertex(
            screen.geometry, clipped_right, clipped_bottom, cursor_u1, cursor_v1);
        const Vertex bottom_left = interpolate_cursor_vertex(screen.geometry, clipped_left, clipped_bottom,
                                                              cursor_u0, cursor_v1);
        const size_t vertex_start = cursor_count * 6;
        cursor_vertices[vertex_start + 0] = bottom_left;
        cursor_vertices[vertex_start + 1] = bottom_right;
        cursor_vertices[vertex_start + 2] = top_right;
        cursor_vertices[vertex_start + 3] = bottom_left;
        cursor_vertices[vertex_start + 4] = top_right;
        cursor_vertices[vertex_start + 5] = top_left;
        cursor_screens[cursor_count++] = index;
    }
    if (cursor_count != 0) {
        D3D11_MAPPED_SUBRESOURCE cursor_mapped{};
        if (SUCCEEDED(context_->Map(cursor_vertex_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                    &cursor_mapped))) {
            std::memcpy(cursor_mapped.pData, cursor_vertices.data(),
                        cursor_count * 6 * sizeof(Vertex));
            context_->Unmap(cursor_vertex_buffer_.Get(), 0);
            ID3D11Buffer* cursor_buffers[] = {cursor_vertex_buffer_.Get()};
            context_->IASetVertexBuffers(0, 1, cursor_buffers, &stride, &offset);
            context_->PSSetShader(cursor_pixel_shader_.Get(), nullptr, 0);
            context_->OMSetDepthStencilState(cursor_depth_state_.Get(), 0);
            const float blend_factor[4] = {};
            context_->OMSetBlendState(cursor_blend_state_.Get(), blend_factor, 0xFFFFFFFFu);
            ID3D11Buffer* cursor_constants[] = {cursor_constant_buffer_.Get()};
            context_->PSSetConstantBuffers(1, 1, cursor_constants);
            for (size_t cursor_index = 0; cursor_index < cursor_count; ++cursor_index) {
                const ScreenDraw& screen = screen_draws_[cursor_screens[cursor_index]];
                CursorConstants constants{};
                constants.u = static_cast<float>(screen.cursor_x) / screen.desktop_width;
                constants.v = static_cast<float>(screen.cursor_y) / screen.desktop_height;
                constants.width = static_cast<float>(screen.cursor_width) / screen.desktop_width;
                constants.height = static_cast<float>(screen.cursor_height) / screen.desktop_height;
                constants.mode = static_cast<uint32_t>(screen.cursor_mode);
                context_->UpdateSubresource(cursor_constant_buffer_.Get(), 0, nullptr, &constants,
                                            0, 0);
                ID3D11ShaderResourceView* textures[] = {screen.cursor_view.Get(),
                                                        screen.live_texture.Get()};
                context_->PSSetShaderResources(0, 2, textures);
                context_->Draw(6, static_cast<UINT>(cursor_index * 6));
            }
            ID3D11ShaderResourceView* unbound[] = {nullptr, nullptr};
            context_->PSSetShaderResources(0, 2, unbound);
            context_->OMSetBlendState(nullptr, blend_factor, 0xFFFFFFFFu);
        }
    }

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
