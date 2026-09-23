#pragma once

#include "imu/fusion.h"
#include "layout/layout.h"
#include "render/camera.h"
#include "render/screen_geometry.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstddef>
#include <array>
#include <string>
#include <vector>

namespace gt {

enum class CursorShapeMode : uint32_t {
    Color = 0,
    MaskedColor = 1,
    Monochrome = 2,
};

struct CursorUpdate {
    bool position_updated = false;
    bool visible = false;
    int x = 0;
    int y = 0;
    uint32_t desktop_width = 0;
    uint32_t desktop_height = 0;
    bool shape_updated = false;
    CursorShapeMode mode = CursorShapeMode::Color;
    uint32_t shape_width = 0;
    uint32_t shape_height = 0;
    uint32_t shape_pitch = 0;
    const uint8_t* shape_pixels = nullptr;
    size_t shape_bytes = 0;
};

class Renderer {
public:
    bool init(HWND hwnd, uint32_t width, uint32_t height, std::string& error);
    void shutdown();
    bool set_layout(const Layout& layout, std::string& error);
    bool set_screen_texture(size_t screen_index, ID3D11ShaderResourceView* texture,
                            std::string& error);
    bool update_screen_cursor(size_t screen_index, const CursorUpdate& update,
                              std::string& error);

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }

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
    Microsoft::WRL::ComPtr<ID3D11PixelShader> cursor_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cursor_vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cursor_constant_buffer_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_state_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> cursor_depth_state_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> cursor_blend_state_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;

    struct ScreenDraw {
        UINT vertex_start = 0;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> label_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> live_texture;
        ScreenLayout layout;
        std::array<ScreenGeometryVertex, 6> geometry{};
        bool cursor_visible = false;
        int cursor_x = 0;
        int cursor_y = 0;
        uint32_t desktop_width = 0;
        uint32_t desktop_height = 0;
        uint32_t cursor_width = 0;
        uint32_t cursor_height = 0;
        CursorShapeMode cursor_mode = CursorShapeMode::Color;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> cursor_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursor_view;
    };
    std::vector<ScreenDraw> screen_draws_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    HANDLE frame_latency_waitable_ = nullptr;
    CameraSigns signs_;
};

}  // namespace gt
