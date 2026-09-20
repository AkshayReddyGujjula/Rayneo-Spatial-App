#pragma once

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
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                    const std::wstring& output_name, std::string& error);
    CapturePollResult poll(CapturedDesktop& frame, std::string& error);
    void reset();

    const std::wstring& output_name() const { return output_name_; }
    uint64_t frames_captured() const { return frames_captured_; }
    uint64_t access_lost_count() const { return access_lost_count_; }

private:
    bool create_duplication(std::string& error);
    bool ensure_copy_texture(ID3D11Texture2D* source, std::string& error);

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
};

}  // namespace gt
