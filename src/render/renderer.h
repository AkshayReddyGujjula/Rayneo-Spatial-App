#pragma once

#include "imu/fusion.h"
#include "render/camera.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace gt {

class Renderer {
public:
    bool init(HWND hwnd, uint32_t width, uint32_t height, std::string& error);
    void shutdown();

    void render(const Quat& head, float fov_horizontal_deg, float time_s);
    void wait_for_frame();
    bool present();
    void set_signs(const CameraSigns& signs) { signs_ = signs; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapchain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_texture_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth_view_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_state_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_disabled_state_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    UINT line_vertex_count_ = 0;
    UINT quad_vertex_start_ = 0;
    UINT quad_vertex_count_ = 0;
    UINT crosshair_vertex_start_ = 0;
    HANDLE frame_latency_waitable_ = nullptr;
    CameraSigns signs_;
};

}  // namespace gt
