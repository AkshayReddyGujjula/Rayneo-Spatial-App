#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

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
    uint32_t width = 0;
    uint32_t height = 0;
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

private:
    bool create_duplication(std::string& error);
    bool ensure_copy_texture(ID3D11Texture2D* source, std::string& error);
    bool ensure_gdi_texture(std::string& error);
    void destroy_gdi_surface();
    CapturePollResult poll_gdi(CapturedDesktop& frame, std::string& error);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy_texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> copy_view_;
    std::wstring output_name_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
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
    bool gdi_cursor_valid_ = false;
    bool gdi_cursor_showing_ = false;
    LONG gdi_cursor_x_ = 0;
    LONG gdi_cursor_y_ = 0;
};

}  // namespace gt
