#include "capture/desktop_duplication.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested, 2,
                                       D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(result)) {
        std::printf("capture_smoketest: D3D11CreateDevice failed (0x%08lx)\n",
                    static_cast<unsigned long>(result));
        return 1;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter))) {
        std::printf("capture_smoketest: could not inspect the D3D adapter\n");
        return 1;
    }
    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(0, &output))) {
        std::printf("capture_smoketest: the D3D adapter has no active outputs\n");
        return 1;
    }
    DXGI_OUTPUT_DESC output_description{};
    if (FAILED(output->GetDesc(&output_description))) {
        std::printf("capture_smoketest: could not read the first output description\n");
        return 1;
    }

    gt::DesktopDuplicator capture;
    std::string error;
    if (!capture.initialize(device.Get(), context.Get(), output_description.DeviceName, error)) {
        std::printf("capture_smoketest: initialization failed: %s\n", error.c_str());
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool dxgi_passed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        gt::CapturedDesktop frame;
        const gt::CapturePollResult poll = capture.poll(frame, error);
        if (poll == gt::CapturePollResult::Frame && frame.texture && frame.width > 0 &&
            frame.height > 0) {
            std::printf("capture_smoketest: PASS (%ls, %ux%u, cursor=%s)\n",
                        output_description.DeviceName, frame.width, frame.height,
                        frame.pointer.visible ? "visible" : "separate cursor not visible");
            dxgi_passed = true;
            break;
        }
        if (poll == gt::CapturePollResult::Failed) {
            std::printf("capture_smoketest: capture failed: %s\n", error.c_str());
            return 1;
        }
        Sleep(10);
    }
    if (!dxgi_passed) {
        std::printf("capture_smoketest: no desktop frame arrived within two seconds\n");
        return 1;
    }

    gt::DesktopDuplicator fallback;
    if (!fallback.initialize(device.Get(), context.Get(), output_description.DeviceName, error,
                             true)) {
        std::printf("capture_smoketest: forced GDI fallback initialization failed: %s\n",
                    error.c_str());
        return 1;
    }
    gt::CapturedDesktop fallback_frame;
    if (fallback.poll(fallback_frame, error) != gt::CapturePollResult::Frame ||
        !fallback_frame.texture || fallback_frame.width == 0 || fallback_frame.height == 0) {
        std::printf("capture_smoketest: forced GDI fallback failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("capture_smoketest: GDI fallback PASS (%ux%u)\n", fallback_frame.width,
                fallback_frame.height);
    return 0;
}
