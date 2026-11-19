#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "render/desktop_rotation.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gt {

enum class CapturePollResult {
    Frame,
    NoFrame,
    Reinitialized,
    Failed,
};

enum class CaptureBackend {
    DesktopDuplication,
    GdiFallback,
};

struct CapturedPointer {
    bool position_updated = false;
    bool visible = false;
    LONG x = 0;
    LONG y = 0;
    bool shape_updated = false;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
    std::vector<uint8_t> pixels;
};

struct CapturedDesktop {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture;
    // Upright desktop size (the output's DesktopCoordinates). Pointer
    // positions are in this space. A rotated DXGI output delivers a surface in
    // scan-out orientation instead: surface_width x surface_height, to be
    // sampled through surface_uv_transform(rotation). GDI is always upright.
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t surface_width = 0;
    uint32_t surface_height = 0;
    DesktopRotation rotation = DesktopRotation::Identity;
    bool desktop_updated = false;
    bool protected_content_masked = false;
    CapturedPointer pointer;
};

class DesktopDuplicator {
public:
    DesktopDuplicator() = default;
    ~DesktopDuplicator();

    DesktopDuplicator(const DesktopDuplicator&) = delete;
    DesktopDuplicator& operator=(const DesktopDuplicator&) = delete;

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                    const std::wstring& output_name, std::string& error,
                    bool force_gdi_fallback = false);
    CapturePollResult poll(CapturedDesktop& frame, std::string& error);
    void reset();

    const std::wstring& output_name() const { return output_name_; }
    uint64_t frames_captured() const { return frames_captured_; }
    uint64_t access_lost_count() const { return access_lost_count_; }
    CaptureBackend backend() const { return backend_; }
    DesktopRotation rotation() const { return rotation_; }

private:
    bool create_duplication(std::string& error);
    bool ensure_copy_texture(ID3D11Texture2D* source, std::string& error);
    bool ensure_gdi_texture(std::string& error);
    void destroy_gdi_surface();
    CapturePollResult poll_gdi(CapturedDesktop& frame, std::string& error);
    bool refresh_gdi_output_rect(std::string& error);
    void update_desktop_size(uint32_t surface_width, uint32_t surface_height);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy_texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> copy_view_;
    std::wstring output_name_;
    uint32_t width_ = 0;   // upright desktop size
    uint32_t height_ = 0;
    uint32_t surface_width_ = 0;  // size of the captured surface (copy texture)
    uint32_t surface_height_ = 0;
    DesktopRotation rotation_ = DesktopRotation::Identity;
    int logged_rotation_ = -1;
    bool logged_size_mismatch_ = false;
    uint64_t frames_captured_ = 0;
    uint64_t access_lost_count_ = 0;
    RECT output_rect_{};
    CaptureBackend backend_ = CaptureBackend::DesktopDuplication;
    bool force_gdi_fallback_ = false;
    bool have_desktop_texture_ = false;

    HDC gdi_desktop_dc_ = nullptr;
    HDC gdi_memory_dc_ = nullptr;
    HBITMAP gdi_bitmap_ = nullptr;
    HGDIOBJ gdi_previous_bitmap_ = nullptr;
    void* gdi_pixels_ = nullptr;
    uint32_t gdi_surface_width_ = 0;
    uint32_t gdi_surface_height_ = 0;
    ULONGLONG gdi_next_rect_check_ms_ = 0;
    bool gdi_cursor_valid_ = false;
    bool gdi_cursor_showing_ = false;
    LONG gdi_cursor_x_ = 0;
    LONG gdi_cursor_y_ = 0;
};

}  // namespace gt
