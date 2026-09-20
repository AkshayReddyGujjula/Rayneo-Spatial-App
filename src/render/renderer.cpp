#include "render/renderer.h"

#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gt {
namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Vertex {
    float x, y, z;
    float r, g, b, a;
};

const char* kShaderSource = R"(
cbuffer Transform : register(b0) {
    float4x4 g_view_proj;
};

struct VSInput {
    float3 pos : POSITION;
    float4 color : COLOR;
};

struct PSInput {
    float4 pos : SV_POSITION;
    float4 color : COLOR;
};

PSInput vs_main(VSInput input) {
    PSInput output;
    output.pos = mul(float4(input.pos, 1.0f), g_view_proj);
    output.color = input.color;
    return output;
}

float4 ps_main(PSInput input) : SV_TARGET {
    return input.color;
}
)";

void add_quad(std::vector<Vertex>& out, float yaw_deg, float distance, float width, float height,
              float r, float g, float b) {
    const float yaw = yaw_deg * kPi / 180.0f;
    const float cx = std::sin(yaw) * distance;
    const float cz = std::cos(yaw) * distance;
    const float rx = std::cos(yaw);
    const float rz = -std::sin(yaw);
    const float hw = width * 0.5f;
    const float hh = height * 0.5f;
    const Vertex v0{cx - rx * hw, -hh, cz - rz * hw, r, g, b, 1.0f};
    const Vertex v1{cx + rx * hw, -hh, cz + rz * hw, r, g, b, 1.0f};
    const Vertex v2{cx + rx * hw, hh, cz + rz * hw, r, g, b, 1.0f};
    const Vertex v3{cx - rx * hw, hh, cz - rz * hw, r, g, b, 1.0f};
    out.push_back(v0);
    out.push_back(v1);
    out.push_back(v2);
    out.push_back(v0);
    out.push_back(v2);
    out.push_back(v3);
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
    };
    if (FAILED(device_->CreateInputLayout(elements, 2, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                          &input_layout_))) {
        error = "input layout creation failed";
        return false;
    }

    std::vector<Vertex> vertices;
    for (int i = -10; i <= 10; ++i) {
        const float t = i * 0.5f;
        vertices.push_back(Vertex{t, 0.0f, -5.0f, 0.22f, 0.22f, 0.26f, 1.0f});
        vertices.push_back(Vertex{t, 0.0f, 5.0f, 0.22f, 0.22f, 0.26f, 1.0f});
        vertices.push_back(Vertex{-5.0f, 0.0f, t, 0.22f, 0.22f, 0.26f, 1.0f});
        vertices.push_back(Vertex{5.0f, 0.0f, t, 0.22f, 0.22f, 0.26f, 1.0f});
    }
    line_vertex_count_ = static_cast<UINT>(vertices.size());

    add_quad(vertices, 0.0f, 2.0f, 1.70f, 0.96f, 0.24f, 0.45f, 0.95f);
    add_quad(vertices, -90.0f, 2.5f, 1.0f, 1.0f, 0.90f, 0.25f, 0.25f);
    add_quad(vertices, 90.0f, 2.5f, 1.0f, 1.0f, 0.25f, 0.85f, 0.35f);
    add_quad(vertices, 180.0f, 3.0f, 1.0f, 1.0f, 0.95f, 0.80f, 0.25f);
    quad_vertex_start_ = line_vertex_count_;
    quad_vertex_count_ = static_cast<UINT>(vertices.size()) - line_vertex_count_;

    crosshair_vertex_start_ = static_cast<UINT>(vertices.size());
    const float s = 0.010f;
    const float z = 0.5f;
    vertices.push_back(Vertex{-s, -s, z, 1.0f, 1.0f, 1.0f, 1.0f});
    vertices.push_back(Vertex{s, -s, z, 1.0f, 1.0f, 1.0f, 1.0f});
    vertices.push_back(Vertex{s, s, z, 1.0f, 1.0f, 1.0f, 1.0f});
    vertices.push_back(Vertex{-s, -s, z, 1.0f, 1.0f, 1.0f, 1.0f});
    vertices.push_back(Vertex{s, s, z, 1.0f, 1.0f, 1.0f, 1.0f});
    vertices.push_back(Vertex{-s, s, z, 1.0f, 1.0f, 1.0f, 1.0f});

    D3D11_BUFFER_DESC vb_desc{};
    vb_desc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex));
    vb_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vb_data{};
    vb_data.pSysMem = vertices.data();
    if (FAILED(device_->CreateBuffer(&vb_desc, &vb_data, &vertex_buffer_))) {
        error = "vertex buffer creation failed";
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
    context_->Draw(line_vertex_count_, 0);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->Draw(quad_vertex_count_, quad_vertex_start_);

    DirectX::XMStoreFloat4x4(&matrix, DirectX::XMMatrixTranspose(projection));
    context_->UpdateSubresource(constant_buffer_.Get(), 0, nullptr, &matrix, 0, 0);
    context_->OMSetDepthStencilState(depth_disabled_state_.Get(), 0);
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
