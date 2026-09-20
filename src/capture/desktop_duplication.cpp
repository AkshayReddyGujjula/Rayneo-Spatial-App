#include "capture/desktop_duplication.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace gt {
namespace {

std::wstring lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), std::towlower);
    return value;
}

std::string hresult_text(const char* operation, HRESULT result) {
    char value[16];
    std::snprintf(value, sizeof(value), "%08lx", static_cast<unsigned long>(result));
    return std::string(operation) + " failed (HRESULT 0x" + value + ")";
}

}  // namespace

DesktopDuplicator::~DesktopDuplicator() {
    reset();
}

bool DesktopDuplicator::initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                                   const std::wstring& output_name, std::string& error,
                                   bool force_gdi_fallback) {
    reset();
    frames_captured_ = 0;
    access_lost_count_ = 0;
    if (device == nullptr || context == nullptr || output_name.empty()) {
        error = "desktop capture requires a D3D11 device, context, and output name";
        return false;
    }
    device_ = device;
    context_ = context;
    output_name_ = output_name;
    force_gdi_fallback_ = force_gdi_fallback;
    return create_duplication(error);
}

bool DesktopDuplicator::create_duplication(std::string& error) {
    duplication_.Reset();
    have_desktop_texture_ = false;
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    HRESULT result = device_.As(&dxgi_device);
    if (SUCCEEDED(result)) result = dxgi_device->GetAdapter(&adapter);
    if (FAILED(result)) {
        error = hresult_text("finding the render adapter", result);
        return false;
    }

    const std::wstring expected = lowercase(output_name_);
    ComPtr<IDXGIOutput> matched;
    DXGI_OUTPUT_DESC matched_description{};
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIOutput> output;
        result = adapter->EnumOutputs(index, &output);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) {
            error = hresult_text("enumerating adapter outputs", result);
            return false;
        }
        DXGI_OUTPUT_DESC description{};
        if (SUCCEEDED(output->GetDesc(&description)) &&
            lowercase(description.DeviceName) == expected) {
            matched = std::move(output);
            matched_description = description;
            break;
        }
    }
    if (!matched) {
        error = "the capture output is not owned by the renderer's D3D11 adapter";
        return false;
    }
    output_rect_ = matched_description.DesktopCoordinates;
    width_ = static_cast<uint32_t>(output_rect_.right - output_rect_.left);
    height_ = static_cast<uint32_t>(output_rect_.bottom - output_rect_.top);

    if (force_gdi_fallback_) {
        backend_ = CaptureBackend::GdiFallback;
        return ensure_gdi_texture(error);
    }

    ComPtr<IDXGIOutput1> output1;
    result = matched.As(&output1);
    if (SUCCEEDED(result)) result = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(result)) {
        backend_ = CaptureBackend::GdiFallback;
        return ensure_gdi_texture(error);
    }
    DXGI_OUTDUPL_DESC description{};
    duplication_->GetDesc(&description);
    width_ = description.ModeDesc.Width;
    height_ = description.ModeDesc.Height;
    if (description.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
        description.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
        duplication_.Reset();
        backend_ = CaptureBackend::GdiFallback;
        return ensure_gdi_texture(error);
    }
    backend_ = CaptureBackend::DesktopDuplication;
    return true;
}

bool DesktopDuplicator::ensure_gdi_texture(std::string& error) {
    if (width_ == 0 || height_ == 0) {
        error = "GDI capture output dimensions are zero";
        return false;
    }
    if (copy_texture_.Get() != nullptr && copy_view_.Get() != nullptr &&
        gdi_bitmap_ != nullptr && gdi_surface_width_ == width_ && gdi_surface_height_ == height_) {
        return true;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width_;
    description.Height = height_;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy_view_.Reset();
    copy_texture_.Reset();
    HRESULT result = device_->CreateTexture2D(&description, nullptr, &copy_texture_);
    if (SUCCEEDED(result)) {
        result = device_->CreateShaderResourceView(copy_texture_.Get(), nullptr, &copy_view_);
    }
    if (FAILED(result)) {
        error = hresult_text("creating the GDI fallback texture", result);
        return false;
    }

    destroy_gdi_surface();
    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = static_cast<LONG>(width_);
    bitmap_info.bmiHeader.biHeight = -static_cast<LONG>(height_);
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    gdi_desktop_dc_ = GetDC(nullptr);
    gdi_memory_dc_ = CreateCompatibleDC(gdi_desktop_dc_);
    gdi_bitmap_ =
        CreateDIBSection(gdi_desktop_dc_, &bitmap_info, DIB_RGB_COLORS, &gdi_pixels_, nullptr, 0);
    if (gdi_desktop_dc_ == nullptr || gdi_memory_dc_ == nullptr || gdi_bitmap_ == nullptr ||
        gdi_pixels_ == nullptr) {
        destroy_gdi_surface();
        error = "could not allocate the GDI fallback capture surface";
        return false;
    }
    gdi_previous_bitmap_ = SelectObject(gdi_memory_dc_, gdi_bitmap_);
    gdi_surface_width_ = width_;
    gdi_surface_height_ = height_;
    return true;
}

void DesktopDuplicator::destroy_gdi_surface() {
    if (gdi_memory_dc_ != nullptr) {
        if (gdi_previous_bitmap_ != nullptr) {
            SelectObject(gdi_memory_dc_, gdi_previous_bitmap_);
        }
        if (gdi_bitmap_ != nullptr) {
            DeleteObject(gdi_bitmap_);
        }
        DeleteDC(gdi_memory_dc_);
    }
    if (gdi_desktop_dc_ != nullptr) {
        ReleaseDC(nullptr, gdi_desktop_dc_);
    }
    gdi_desktop_dc_ = nullptr;
    gdi_memory_dc_ = nullptr;
    gdi_bitmap_ = nullptr;
    gdi_previous_bitmap_ = nullptr;
    gdi_pixels_ = nullptr;
    gdi_surface_width_ = 0;
    gdi_surface_height_ = 0;
    gdi_cursor_valid_ = false;
}

CapturePollResult DesktopDuplicator::poll_gdi(CapturedDesktop& frame, std::string& error) {
    if (gdi_memory_dc_ == nullptr || gdi_bitmap_ == nullptr || gdi_pixels_ == nullptr) {
        error = "the GDI fallback capture surface is not initialized";
        return CapturePollResult::Failed;
    }
    const BOOL copied = BitBlt(gdi_memory_dc_, 0, 0, static_cast<int>(width_),
                               static_cast<int>(height_), gdi_desktop_dc_, output_rect_.left,
                               output_rect_.top, SRCCOPY | CAPTUREBLT);

    bool cursor_showing = false;
    LONG cursor_x = 0;
    LONG cursor_y = 0;
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    if (copied && GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING) != 0 &&
        cursor.hCursor != nullptr) {
        ICONINFO icon{};
        if (GetIconInfo(cursor.hCursor, &icon)) {
            cursor_showing = true;
            cursor_x = cursor.ptScreenPos.x;
            cursor_y = cursor.ptScreenPos.y;
            const int x = cursor.ptScreenPos.x - output_rect_.left -
                          static_cast<int>(icon.xHotspot);
            const int y = cursor.ptScreenPos.y - output_rect_.top -
                          static_cast<int>(icon.yHotspot);
            DrawIconEx(gdi_memory_dc_, x, y, cursor.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
            if (icon.hbmColor != nullptr) DeleteObject(icon.hbmColor);
            if (icon.hbmMask != nullptr) DeleteObject(icon.hbmMask);
        }
    }
    if (!copied) {
        error = "BitBlt failed while capturing the fallback desktop";
        return CapturePollResult::Failed;
    }

    const bool cursor_changed = !gdi_cursor_valid_ || cursor_showing != gdi_cursor_showing_ ||
                                cursor_x != gdi_cursor_x_ || cursor_y != gdi_cursor_y_;
    gdi_cursor_valid_ = true;
    gdi_cursor_showing_ = cursor_showing;
    gdi_cursor_x_ = cursor_x;
    gdi_cursor_y_ = cursor_y;

    ID3D11ShaderResourceView* unbound[] = {nullptr, nullptr};
    context_->PSSetShaderResources(0, 2, unbound);
    context_->UpdateSubresource(copy_texture_.Get(), 0, nullptr, gdi_pixels_, width_ * 4, 0);

    frame.texture = copy_view_;
    frame.width = width_;
    frame.height = height_;
    frame.desktop_updated = true;
    frame.pointer.position_updated = cursor_changed;
    frame.pointer.visible = false;
    ++frames_captured_;
    return CapturePollResult::Frame;
}

bool DesktopDuplicator::ensure_copy_texture(ID3D11Texture2D* source, std::string& error) {
    D3D11_TEXTURE2D_DESC source_description{};
    source->GetDesc(&source_description);
    if (copy_texture_) {
        D3D11_TEXTURE2D_DESC existing{};
        copy_texture_->GetDesc(&existing);
        if (existing.Width == source_description.Width && existing.Height == source_description.Height &&
            existing.Format == source_description.Format) {
            return true;
        }
    }

    copy_view_.Reset();
    copy_texture_.Reset();
    D3D11_TEXTURE2D_DESC copy = source_description;
    copy.MipLevels = 1;
    copy.ArraySize = 1;
    copy.SampleDesc.Count = 1;
    copy.SampleDesc.Quality = 0;
    copy.Usage = D3D11_USAGE_DEFAULT;
    copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy.CPUAccessFlags = 0;
    copy.MiscFlags = 0;
    HRESULT result = device_->CreateTexture2D(&copy, nullptr, &copy_texture_);
    if (SUCCEEDED(result)) {
        result = device_->CreateShaderResourceView(copy_texture_.Get(), nullptr, &copy_view_);
    }
    if (FAILED(result)) {
        error = hresult_text("creating the desktop copy texture", result);
        copy_texture_.Reset();
        return false;
    }
    width_ = copy.Width;
    height_ = copy.Height;
    return true;
}

CapturePollResult DesktopDuplicator::poll(CapturedDesktop& frame, std::string& error) {
    frame = CapturedDesktop{};
    if (backend_ == CaptureBackend::GdiFallback) {
        return poll_gdi(frame, error);
    }
    if (!duplication_) {
        if (!create_duplication(error)) return CapturePollResult::Failed;
        return CapturePollResult::Reinitialized;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT result = duplication_->AcquireNextFrame(0, &info, &resource);
    if (result == DXGI_ERROR_WAIT_TIMEOUT) return CapturePollResult::NoFrame;
    if (result == DXGI_ERROR_ACCESS_LOST) {
        ++access_lost_count_;
        duplication_.Reset();
        if (!create_duplication(error)) return CapturePollResult::Failed;
        return CapturePollResult::Reinitialized;
    }
    if (FAILED(result)) {
        error = hresult_text("AcquireNextFrame", result);
        return CapturePollResult::Failed;
    }

    ComPtr<ID3D11Texture2D> source;
    result = resource.As(&source);
    if (FAILED(result) || !ensure_copy_texture(source.Get(), error)) {
        duplication_->ReleaseFrame();
        if (FAILED(result)) error = hresult_text("reading the duplicated desktop texture", result);
        return CapturePollResult::Failed;
    }
    frame.texture = copy_view_;
    frame.width = width_;
    frame.height = height_;
    frame.desktop_updated = info.LastPresentTime.QuadPart != 0 || !have_desktop_texture_;
    frame.protected_content_masked = info.ProtectedContentMaskedOut != FALSE;
    if (frame.desktop_updated) {
        ID3D11ShaderResourceView* unbound[] = {nullptr, nullptr};
        context_->PSSetShaderResources(0, 2, unbound);
        context_->CopyResource(copy_texture_.Get(), source.Get());
        have_desktop_texture_ = true;
    }
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        frame.pointer.position_updated = true;
        frame.pointer.visible = info.PointerPosition.Visible != FALSE;
        frame.pointer.x = info.PointerPosition.Position.x;
        frame.pointer.y = info.PointerPosition.Position.y;
    }
    if (info.PointerShapeBufferSize != 0) {
        constexpr UINT kMaximumPointerBytes = 4 * 1024 * 1024;
        if (info.PointerShapeBufferSize > kMaximumPointerBytes) {
            duplication_->ReleaseFrame();
            error = "desktop pointer shape exceeds the 4 MiB safety limit";
            return CapturePollResult::Failed;
        }
        frame.pointer.pixels.resize(info.PointerShapeBufferSize);
        UINT required = 0;
        result = duplication_->GetFramePointerShape(
            static_cast<UINT>(frame.pointer.pixels.size()), frame.pointer.pixels.data(), &required,
            &frame.pointer.shape);
        if (result == DXGI_ERROR_MORE_DATA && required <= kMaximumPointerBytes) {
            frame.pointer.pixels.resize(required);
            result = duplication_->GetFramePointerShape(
                required, frame.pointer.pixels.data(), &required, &frame.pointer.shape);
        }
        if (FAILED(result)) {
            duplication_->ReleaseFrame();
            error = hresult_text("GetFramePointerShape", result);
            return CapturePollResult::Failed;
        }
        frame.pointer.pixels.resize(required);
        frame.pointer.shape_updated = true;
    }

    result = duplication_->ReleaseFrame();
    if (FAILED(result)) {
        error = hresult_text("ReleaseFrame", result);
        return CapturePollResult::Failed;
    }
    ++frames_captured_;
    return CapturePollResult::Frame;
}

void DesktopDuplicator::reset() {
    destroy_gdi_surface();
    duplication_.Reset();
    copy_view_.Reset();
    copy_texture_.Reset();
    context_.Reset();
    device_.Reset();
    output_name_.clear();
    width_ = 0;
    height_ = 0;
    have_desktop_texture_ = false;
}

}  // namespace gt
