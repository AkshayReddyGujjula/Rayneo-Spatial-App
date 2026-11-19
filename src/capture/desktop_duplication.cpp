#include "capture/desktop_duplication.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace gt {

// desktop_rotation_from_dxgi takes the raw enum value to stay DXGI-free.
static_assert(DXGI_MODE_ROTATION_UNSPECIFIED == 0 && DXGI_MODE_ROTATION_IDENTITY == 1 &&
              DXGI_MODE_ROTATION_ROTATE90 == 2 && DXGI_MODE_ROTATION_ROTATE180 == 3 &&
              DXGI_MODE_ROTATION_ROTATE270 == 4);

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
    const auto find_output = [&](IDXGIAdapter* candidate) -> HRESULT {
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIOutput> output;
            const HRESULT enumerated = candidate->EnumOutputs(index, &output);
            if (enumerated == DXGI_ERROR_NOT_FOUND) return S_OK;
            if (FAILED(enumerated)) return enumerated;
            DXGI_OUTPUT_DESC description{};
            if (SUCCEEDED(output->GetDesc(&description)) &&
                lowercase(description.DeviceName) == expected) {
                matched = std::move(output);
                matched_description = description;
                return S_OK;
            }
        }
    };
    result = find_output(adapter.Get());
    if (FAILED(result)) {
        error = hresult_text("enumerating adapter outputs", result);
        return false;
    }
    if (!matched) {
        // The device's adapter object lists only the outputs that existed when
        // its factory was created, so a virtual display added mid-session
        // (layout hot reload) is invisible to it. Re-enumerate the same
        // physical adapter (matched by LUID) through a fresh factory.
        DXGI_ADAPTER_DESC device_adapter{};
        ComPtr<IDXGIFactory1> factory;
        result = adapter->GetDesc(&device_adapter);
        if (SUCCEEDED(result)) result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(result)) {
            error = hresult_text("refreshing the render adapter's outputs", result);
            return false;
        }
        for (UINT index = 0; !matched; ++index) {
            ComPtr<IDXGIAdapter1> fresh;
            if (factory->EnumAdapters1(index, &fresh) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 fresh_desc{};
            if (FAILED(fresh->GetDesc1(&fresh_desc)) ||
                fresh_desc.AdapterLuid.LowPart != device_adapter.AdapterLuid.LowPart ||
                fresh_desc.AdapterLuid.HighPart != device_adapter.AdapterLuid.HighPart) {
                continue;
            }
            result = find_output(fresh.Get());
            if (FAILED(result)) {
                error = hresult_text("enumerating refreshed adapter outputs", result);
                return false;
            }
        }
    }
    if (!matched) {
        error = "the capture output is not owned by the renderer's D3D11 adapter";
        return false;
    }
    output_rect_ = matched_description.DesktopCoordinates;
    width_ = static_cast<uint32_t>(output_rect_.right - output_rect_.left);
    height_ = static_cast<uint32_t>(output_rect_.bottom - output_rect_.top);

    // DesktopCoordinates are in desktop space, so for a rotated output this is
    // already the upright size (1080x1920 for a 1920x1080 mode at DMDO_90).
    rotation_ = DesktopRotation::Identity;
    surface_width_ = width_;
    surface_height_ = height_;
    logged_size_mismatch_ = false;

    if (force_gdi_fallback_) {
        backend_ = CaptureBackend::GdiFallback;
        return ensure_gdi_texture(error);
    }

    ComPtr<IDXGIOutput1> output1;
    result = matched.As(&output1);
    const char* operation = SUCCEEDED(result) ? "DuplicateOutput" : "querying IDXGIOutput1";
    if (SUCCEEDED(result)) result = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(result)) {
        std::printf("  capture output %ls: %s, using CPU GDI capture fallback\n",
                    output_name_.c_str(), hresult_text(operation, result).c_str());
        backend_ = CaptureBackend::GdiFallback;
        return ensure_gdi_texture(error);
    }
    // Re-read on every (re)creation: a rotation change is a mode change, which
    // invalidates the duplication (AcquireNextFrame returns
    // DXGI_ERROR_ACCESS_LOST, per the AcquireNextFrame docs), and poll() lands
    // here again, so a stale rotation can never survive a re-duplication.
    // The frames keep arriving in the unrotated scan-out orientation; the
    // renderer rotates them through surface_uv_transform (desktop_rotation.h).
    DXGI_OUTDUPL_DESC description{};
    duplication_->GetDesc(&description);
    rotation_ = desktop_rotation_from_dxgi(static_cast<int>(description.Rotation));
    // Provisional until the first frame's surface size (update_desktop_size).
    // ModeDesc is the scan-out mode, so it is swapped like the surface.
    if (description.ModeDesc.Width != 0 && description.ModeDesc.Height != 0) {
        surface_width_ = description.ModeDesc.Width;
        surface_height_ = description.ModeDesc.Height;
        desktop_size_from_surface(rotation_, surface_width_, surface_height_, width_, height_);
    } else {
        desktop_size_from_surface(rotation_, width_, height_, surface_width_, surface_height_);
    }
    if (logged_rotation_ != static_cast<int>(rotation_)) {
        std::printf("  capture output %ls: DXGI duplication, rotation %d deg (DXGI_MODE_ROTATION "
                    "%d), mode %ux%u, desktop coordinates %ldx%ld\n",
                    output_name_.c_str(), desktop_rotation_degrees(rotation_),
                    static_cast<int>(description.Rotation), description.ModeDesc.Width,
                    description.ModeDesc.Height, output_rect_.right - output_rect_.left,
                    output_rect_.bottom - output_rect_.top);
        logged_rotation_ = static_cast<int>(rotation_);
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
    // GDI has no ACCESS_LOST: notice a mode change (resize, rotation, move)
    // by re-reading the monitor rectangle about once a second.
    const ULONGLONG now_ms = GetTickCount64();
    if (now_ms >= gdi_next_rect_check_ms_) {
        gdi_next_rect_check_ms_ = now_ms + 1000;
        if (!refresh_gdi_output_rect(error)) return CapturePollResult::Failed;
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
    frame.surface_width = width_;
    frame.surface_height = height_;
    frame.rotation = DesktopRotation::Identity;  // GDI copies desktop coordinates: upright
    frame.desktop_updated = true;
    frame.pointer.position_updated = cursor_changed;
    frame.pointer.visible = false;
    ++frames_captured_;
    return CapturePollResult::Frame;
}

namespace {

struct MonitorSearch {
    const std::wstring* name;
    RECT rect;
    bool found;
};

BOOL CALLBACK find_monitor_rect(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* search = reinterpret_cast<MonitorSearch*>(data);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) && _wcsicmp(info.szDevice, search->name->c_str()) == 0) {
        search->rect = info.rcMonitor;
        search->found = true;
        return FALSE;
    }
    return TRUE;
}

}  // namespace

bool DesktopDuplicator::refresh_gdi_output_rect(std::string& error) {
    MonitorSearch search{&output_name_, RECT{}, false};
    EnumDisplayMonitors(nullptr, nullptr, find_monitor_rect, reinterpret_cast<LPARAM>(&search));
    if (!search.found) return true;  // transiently absent: keep the last surface
    const RECT& r = search.rect;
    if (r.left == output_rect_.left && r.top == output_rect_.top && r.right == output_rect_.right &&
        r.bottom == output_rect_.bottom) {
        return true;
    }
    if (r.right <= r.left || r.bottom <= r.top) return true;
    const uint32_t width = static_cast<uint32_t>(r.right - r.left);
    const uint32_t height = static_cast<uint32_t>(r.bottom - r.top);
    if (width != width_ || height != height_) {
        std::printf("  capture output %ls: GDI desktop changed %ux%u -> %ux%u\n",
                    output_name_.c_str(), width_, height_, width, height);
    }
    output_rect_ = r;
    width_ = width;
    height_ = height;
    surface_width_ = width;
    surface_height_ = height;
    gdi_cursor_valid_ = false;
    return ensure_gdi_texture(error);
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
    return true;
}

// The duplicated surface is authoritative for size: it is the unrotated
// scan-out image in physical pixels, while DXGI_OUTPUT_DESC.DesktopCoordinates
// are DPI-virtualised in a process that is not per-monitor aware (measured:
// capture_smoketest saw 1646x1029 coordinates for a 2880x1800 surface). Pointer
// positions are physical desktop pixels, so the upright size is the surface
// size with the axes swapped for 90/270. Recomputed every frame because a
// rotation change re-duplicates without necessarily changing the surface size.
void DesktopDuplicator::update_desktop_size(uint32_t surface_width, uint32_t surface_height) {
    surface_width_ = surface_width;
    surface_height_ = surface_height;
    desktop_size_from_surface(rotation_, surface_width, surface_height, width_, height_);
    // DPI virtualisation scales DesktopCoordinates but never turns them on
    // their side: an orientation disagreement means the rotation contract in
    // desktop_rotation.h does not hold for this driver. Say so once.
    const LONG coords_w = output_rect_.right - output_rect_.left;
    const LONG coords_h = output_rect_.bottom - output_rect_.top;
    if (!logged_size_mismatch_ && coords_w > 0 && coords_h > 0 && coords_w != coords_h &&
        width_ != height_ && ((coords_w > coords_h) != (width_ > height_))) {
        std::printf("  capture output %ls: WARNING duplicated surface %ux%u at rotation %d deg "
                    "gives a %ux%u desktop, but the output's desktop coordinates are %ldx%ld\n",
                    output_name_.c_str(), surface_width, surface_height,
                    desktop_rotation_degrees(rotation_), width_, height_, coords_w, coords_h);
        logged_size_mismatch_ = true;
    }
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
    {
        D3D11_TEXTURE2D_DESC source_description{};
        source->GetDesc(&source_description);
        update_desktop_size(source_description.Width, source_description.Height);
    }
    frame.texture = copy_view_;
    frame.width = width_;
    frame.height = height_;
    frame.surface_width = surface_width_;
    frame.surface_height = surface_height_;
    frame.rotation = rotation_;
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
    surface_width_ = 0;
    surface_height_ = 0;
    rotation_ = DesktopRotation::Identity;
    logged_rotation_ = -1;
    logged_size_mismatch_ = false;
    gdi_next_rect_check_ms_ = 0;
    have_desktop_texture_ = false;
}

}  // namespace gt
