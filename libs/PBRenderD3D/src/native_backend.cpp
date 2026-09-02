#include "presentation_backend.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/checked_integer.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwchar>
#include <limits>

namespace pbrenderd3d
{
namespace
{
using Microsoft::WRL::ComPtr;

class OwnedHandle
{
public:
    OwnedHandle() = default;
    ~OwnedHandle()
    {
        Reset();
    }
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept
    {
        return handle_;
    }
    void Reset(const HANDLE replacement = nullptr) noexcept
    {
        if (handle_ != nullptr)
        {
            CloseHandle(handle_);
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_ = nullptr;
};

[[nodiscard]] PresentationStatus NativeError(const HRESULT result, const PresentationStage stage) noexcept
{
    const auto code = result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DEVICE_HUNG
                          ? PresentationErrorCode::DeviceLost
                          : (result == E_OUTOFMEMORY ? PresentationErrorCode::OutOfMemory : PresentationErrorCode::NativeFailure);
    return PresentationStatus::Failure(code, stage, static_cast<std::int32_t>(result));
}

[[nodiscard]] PresentationStatus LastWindowsError(const PresentationStage stage) noexcept
{
    return PresentationStatus::Failure(PresentationErrorCode::NativeFailure, stage, static_cast<std::int32_t>(GetLastError()));
}

class NativeBackend final : public PresentationBackend
{
public:
    explicit NativeBackend(const NativeBackendTestOptions& options) noexcept : options_(options)
    {
        LARGE_INTEGER frequency{};
        if (QueryPerformanceFrequency(&frequency))
        {
            frequency_ = frequency.QuadPart;
        }
        wake_.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        wakeError_ = wake_.Get() == nullptr ? GetLastError() : ERROR_SUCCESS;
        diagnostics_.warp = options.warp;
    }

    [[nodiscard]] std::int64_t GetQpcFrequency() const noexcept override
    {
        return frequency_;
    }
    [[nodiscard]] std::int64_t NowQpc() const noexcept override
    {
        LARGE_INTEGER counter{};
        return QueryPerformanceCounter(&counter) ? counter.QuadPart : -1;
    }

    [[nodiscard]] PresentationStatus Initialize(const DataWindowConfig& config) noexcept override
    {
        config_ = config;
        if (wake_.Get() == nullptr)
        {
            return PresentationStatus::Failure(PresentationErrorCode::NativeFailure, PresentationStage::WakeEvent, static_cast<std::int32_t>(wakeError_));
        }
        if (frequency_ <= 0)
        {
            return PresentationStatus::Failure(PresentationErrorCode::NativeFailure, PresentationStage::Thread);
        }
        if (ShouldFail(PresentationStage::WakeEvent))
        {
            return InjectedFailure(PresentationStage::WakeEvent);
        }
        if (!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        {
            return PresentationStatus::Failure(PresentationErrorCode::DpiAwarenessRequired, PresentationStage::DpiAwareness);
        }
        contract_.perMonitorV2 = true;
        instance_ = GetModuleHandleW(nullptr);
        const int characters = swprintf_s(className_.data(), className_.size(), L"PixelBridge.Data.%lu.%p", GetCurrentProcessId(), static_cast<void*>(this));
        if (characters <= 0)
        {
            return PresentationStatus::Failure(PresentationErrorCode::InternalError, PresentationStage::WindowClass);
        }
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &WindowProcedure;
        windowClass.hInstance = instance_;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = className_.data();
        if (RegisterClassExW(&windowClass) == 0)
        {
            return LastWindowsError(PresentationStage::WindowClass);
        }
        classRegistered_ = true;
        if (ShouldFail(PresentationStage::WindowClass))
        {
            return InjectedFailure(PresentationStage::WindowClass);
        }
        PhysicalPoint origin{};
        if (config_.clientOrigin)
        {
            origin = *config_.clientOrigin;
        }
        else
        {
            MONITORINFO monitorInfo{};
            monitorInfo.cbSize = sizeof(monitorInfo);
            if (!GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitorInfo))
            {
                return LastWindowsError(PresentationStage::Environment);
            }
            const std::int64_t monitorWidth = static_cast<std::int64_t>(monitorInfo.rcMonitor.right) - monitorInfo.rcMonitor.left;
            const std::int64_t monitorHeight = static_cast<std::int64_t>(monitorInfo.rcMonitor.bottom) - monitorInfo.rcMonitor.top;
            if (monitorWidth < config_.width || monitorHeight < config_.height)
            {
                return PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::Environment);
            }
            origin.x = static_cast<std::int32_t>(static_cast<std::int64_t>(monitorInfo.rcMonitor.left) + (monitorWidth - config_.width) / 2);
            origin.y = static_cast<std::int32_t>(static_cast<std::int64_t>(monitorInfo.rcMonitor.top) + (monitorHeight - config_.height) / 2);
        }
        const DWORD extendedStyle = WS_EX_NOACTIVATE | (config_.topmost ? WS_EX_TOPMOST : 0U);
        const HWND window = CreateWindowExW(extendedStyle, className_.data(), L"PixelBridge Data Window", WS_POPUP, origin.x, origin.y, static_cast<int>(config_.width),
                                            static_cast<int>(config_.height), nullptr, nullptr, instance_, this);
        if (window == nullptr)
        {
            return LastWindowsError(PresentationStage::Window);
        }
        window_.store(window);
        if (ShouldFail(PresentationStage::Window))
        {
            return InjectedFailure(PresentationStage::Window);
        }
        auto status = RefreshFactory();
        if (!status)
        {
            return status;
        }
        const auto environment = PollEnvironment();
        if (!environment)
        {
            return environment.Error();
        }
        if (!environment.Value().adapterAvailable)
        {
            return PresentationStatus::Failure(PresentationErrorCode::NativeFailure, PresentationStage::Adapter);
        }
        if (ShouldFail(PresentationStage::Adapter))
        {
            return InjectedFailure(PresentationStage::Adapter);
        }
        status = CreateGraphics(environment.Value());
        if (!status)
        {
            return status;
        }
        ShowWindow(window, SW_SHOWNOACTIVATE);
        return PresentationStatus::Success();
    }

    [[nodiscard]] PresentationResult<WindowEnvironment> PollEnvironment() noexcept override
    {
        using Result = PresentationResult<WindowEnvironment>;
        PumpMessages();
        WindowEnvironment environment;
        environment.closed = closeRequested_;
        const HWND window = window_.load();
        if (window == nullptr || closeRequested_)
        {
            environment.closed = true;
            return Result::Success(environment);
        }
        if (suggestedDpiRect_)
        {
            const RECT suggested = *suggestedDpiRect_;
            suggestedDpiRect_.reset();
            if (!SetWindowPos(window, nullptr, suggested.left, suggested.top, static_cast<int>(config_.width), static_cast<int>(config_.height),
                              SWP_NOACTIVATE | SWP_NOZORDER))
            {
                return Result::Failure(LastWindowsError(PresentationStage::Environment));
            }
        }
        RECT client{};
        POINT origin{};
        if (!GetClientRect(window, &client) || !ClientToScreen(window, &origin))
        {
            return Result::Failure(LastWindowsError(PresentationStage::Environment));
        }
        const std::int64_t width = static_cast<std::int64_t>(client.right) - client.left;
        const std::int64_t height = static_cast<std::int64_t>(client.bottom) - client.top;
        if (width < 0 || height < 0 || width > std::numeric_limits<std::uint32_t>::max() || height > std::numeric_limits<std::uint32_t>::max())
        {
            return Result::Failure(PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::Environment));
        }
        environment.clientOrigin = {origin.x, origin.y};
        environment.clientWidth = static_cast<std::uint32_t>(width);
        environment.clientHeight = static_cast<std::uint32_t>(height);
        environment.dpi = GetDpiForWindow(window);
        environment.minimized = IsIconic(window) != FALSE;
        environment.modeChangeSerial = modeChangeSerial_;
        environment.dpiChangeSerial = dpiChangeSerial_;
        const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo))
        {
            // A display can temporarily disappear during a mode/topology
            // change. No new resources or payload are admitted in this state.
            return Result::Success(environment);
        }
        environment.monitorIdentity = reinterpret_cast<std::uintptr_t>(monitor);
        environment.singleMonitor = width > 0 && height > 0 && origin.x >= monitorInfo.rcMonitor.left && origin.y >= monitorInfo.rcMonitor.top &&
                                    static_cast<std::int64_t>(origin.x) + width <= monitorInfo.rcMonitor.right &&
                                    static_cast<std::int64_t>(origin.y) + height <= monitorInfo.rcMonitor.bottom;
        std::copy_n(monitorInfo.szDevice, environment.displayName.size(), environment.displayName.data());
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsExW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0))
        {
            environment.modeWidth = mode.dmPelsWidth;
            environment.modeHeight = mode.dmPelsHeight;
            environment.modeFrequency = mode.dmDisplayFrequency;
            environment.modeOrientation = mode.dmDisplayOrientation;
        }
        else
        {
            environment.singleMonitor = false;
        }
        const auto status = RefreshFactory();
        if (!status)
        {
            return Result::Failure(status);
        }
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 description{};
        if (FindAdapter(monitor, adapter, description))
        {
            environment.adapterAvailable = true;
            environment.adapterLuidLow = description.AdapterLuid.LowPart;
            environment.adapterLuidHigh = description.AdapterLuid.HighPart;
            std::copy_n(description.Description, environment.adapterDescription.size(), environment.adapterDescription.data());
        }
        if (occluded_ && swapChain_ && !environment.minimized && environment.singleMonitor && !cancelled_.load())
        {
            const HRESULT visible = swapChain_->Present(0, DXGI_PRESENT_TEST);
            if (visible == S_OK)
            {
                occluded_ = false;
            }
            else if (visible != DXGI_STATUS_OCCLUDED)
            {
                return Result::Failure(DeviceError(visible, PresentationStage::Present));
            }
        }
        environment.occluded = occluded_;
        return Result::Success(environment);
    }

    [[nodiscard]] PresentationStatus Reconfigure(const WindowEnvironment& environment) noexcept override
    {
        if (environment.minimized || environment.clientWidth == 0 || environment.clientHeight == 0 || !environment.adapterAvailable ||
            !environment.singleMonitor)
        {
            return PresentationStatus::Success();
        }
        const std::uint64_t pixels = static_cast<std::uint64_t>(environment.clientWidth) * environment.clientHeight;
        if (environment.clientWidth > 16384 || environment.clientHeight > 16384 || pixels > config_.maximumFrameBytes / 4)
        {
            // Also gate a cross-adapter rebuild: a user-sized HWND is not an
            // authorization to allocate arbitrary textures on another GPU.
            return PresentationStatus::Success();
        }
        const bool adapterChanged = deviceMonitorLuid_.LowPart != environment.adapterLuidLow || deviceMonitorLuid_.HighPart != environment.adapterLuidHigh;
        if (adapterChanged)
        {
            const auto status = DrainGpu();
            if (!status)
            {
                return status;
            }
            ReleaseGraphics();
            return CreateGraphics(environment);
        }
        if (environment.clientWidth == contract_.bufferWidth && environment.clientHeight == contract_.bufferHeight)
        {
            const auto drained = DrainGpu();
            if (!drained)
            {
                return drained;
            }
            sourceTexture_.Reset();
            return RefreshContract();
        }
        auto status = DrainGpu();
        if (!status)
        {
            return status;
        }
        context_->ClearState();
        sourceTexture_.Reset();
        staging_.Reset();
        backBuffer_.Reset();
        context_->Flush();
        const HRESULT resized = swapChain_->ResizeBuffers(config_.bufferCount, environment.clientWidth, environment.clientHeight, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                          DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        if (FAILED(resized))
        {
            return DeviceError(resized, PresentationStage::Resize);
        }
        status = CreateBackBuffer();
        return status ? RefreshContract() : status;
    }

    [[nodiscard]] PresentationContract GetContract() const noexcept override
    {
        return contract_;
    }

    [[nodiscard]] BackendDiagnostics GetDiagnostics() const noexcept override
    {
        BackendDiagnostics result = diagnostics_;
        result.liveOwnedHandles = static_cast<std::uint64_t>(wake_.Get() != nullptr) + static_cast<std::uint64_t>(frameLatency_.Get() != nullptr);
        result.liveGraphicsObjects = static_cast<std::uint64_t>(factory_ != nullptr) + static_cast<std::uint64_t>(device_ != nullptr) +
                                     static_cast<std::uint64_t>(context_ != nullptr) + static_cast<std::uint64_t>(swapChain_ != nullptr) +
                                     static_cast<std::uint64_t>(backBuffer_ != nullptr) + static_cast<std::uint64_t>(sourceTexture_ != nullptr) +
                                     static_cast<std::uint64_t>(staging_ != nullptr) +
                                     static_cast<std::uint64_t>(completion_ != nullptr) + static_cast<std::uint64_t>(infoQueue_ != nullptr);
        return result;
    }

    [[nodiscard]] BackendWaitResult Wait(const bool requestFramePermit, const std::uint32_t timeoutMilliseconds) noexcept override
    {
        const std::array<HANDLE, 2> handles{wake_.Get(), frameLatency_.Get()};
        if (requestFramePermit && frameLatency_.Get() == nullptr)
        {
            return {BackendWake::Failed, PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::Wait)};
        }
        const DWORD count = requestFramePermit ? 2u : 1u;
        const DWORD result = MsgWaitForMultipleObjectsEx(count, handles.data(), timeoutMilliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        const DWORD waitError = result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
        PumpMessages();
        if (result == WAIT_OBJECT_0 + 1 && requestFramePermit)
        {
            pbprotocol::SaturatingIncrementUnsigned(diagnostics_.framePermits);
            return {BackendWake::FramePermit, {}};
        }
        if (result == WAIT_TIMEOUT)
        {
            return {BackendWake::Timeout, {}};
        }
        if (result == WAIT_FAILED)
        {
            return {BackendWake::Failed,
                    PresentationStatus::Failure(PresentationErrorCode::WaitFailed, PresentationStage::Wait, static_cast<std::int32_t>(waitError))};
        }
        return {BackendWake::Message, {}};
    }

    [[nodiscard]] PresentationStatus Upload(const std::span<const std::byte> pixels) noexcept override
    {
        if (!device_ || !context_ || !backBuffer_ || contract_.bufferWidth != config_.width || contract_.bufferHeight != config_.height ||
            pixels.size() != static_cast<std::size_t>(config_.width) * config_.height * 4)
        {
            return PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::Upload);
        }
        D3D11_TEXTURE2D_DESC description{};
        description.Width = config_.width;
        description.Height = config_.height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initialData{};
        initialData.pSysMem = pixels.data();
        initialData.SysMemPitch = config_.width * 4;
        ComPtr<ID3D11Texture2D> sourceTexture;
        const HRESULT created = device_->CreateTexture2D(&description, &initialData, sourceTexture.GetAddressOf());
        if (FAILED(created))
        {
            return DeviceError(created, PresentationStage::Upload);
        }
        pbprotocol::SaturatingIncrementUnsigned(diagnostics_.immutableSourceCreations);
        if (options_.verifyUploads)
        {
            if (!staging_)
            {
                return PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::Readback);
            }
            context_->CopyResource(staging_.Get(), sourceTexture.Get());
            const auto drained = DrainGpu();
            if (!drained)
            {
                return drained;
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT mappedResult = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
            if (FAILED(mappedResult))
            {
                return DeviceError(mappedResult, PresentationStage::Readback);
            }
            const std::size_t rowBytes = static_cast<std::size_t>(config_.width) * 4;
            bool identical = mapped.pData != nullptr && mapped.RowPitch >= rowBytes;
            pbprotocol::Blake3Hasher hasher;
            for (std::size_t row = 0; identical && row < config_.height; row++)
            {
                const auto* const gpuRow = static_cast<const std::byte*>(mapped.pData) + row * mapped.RowPitch;
                identical = std::equal(gpuRow, gpuRow + rowBytes, pixels.data() + row * rowBytes);
                hasher.Update(std::span(gpuRow, rowBytes));
            }
            context_->Unmap(staging_.Get(), 0);
            if (!identical)
            {
                return PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::Readback);
            }
            pbprotocol::SaturatingIncrementUnsigned(diagnostics_.verifiedUploads);
            diagnostics_.lastSourceReadbackBlake3 = hasher.Finalize();
            diagnostics_.sourceReadbackBlake3Valid = true;
        }
        const auto debug = CheckDebugLayer();
        if (!debug)
        {
            return debug;
        }
        sourceTexture_ = std::move(sourceTexture);
        return PresentationStatus::Success();
    }

    [[nodiscard]] BackendPresentResult Present() noexcept override
    {
        if (!context_ || !swapChain_ || !backBuffer_ || !sourceTexture_)
        {
            return {PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::Present),
                pbpresenttiming::PresentOutcome::Failure, std::nullopt};
        }
        context_->CopyResource(backBuffer_.Get(), sourceTexture_.Get());
        pbprotocol::SaturatingIncrementUnsigned(diagnostics_.sourceCopiesToBackBuffer);
        pbprotocol::SaturatingIncrementUnsigned(diagnostics_.presentCalls);
        const HRESULT result = swapChain_->Present(1, 0);
        if (result == DXGI_STATUS_OCCLUDED)
        {
            occluded_ = true;
            return {{}, pbpresenttiming::PresentOutcome::Occluded, std::nullopt};
        }
        if (FAILED(result))
        {
            return {DeviceError(result, PresentationStage::Present), pbpresenttiming::PresentOutcome::Failure, std::nullopt};
        }
        UINT presentId = 0;
        const HRESULT idResult = swapChain_->GetLastPresentCount(&presentId);
        if (idResult == DXGI_ERROR_DEVICE_REMOVED || idResult == DXGI_ERROR_DEVICE_RESET || idResult == DXGI_ERROR_DEVICE_HUNG)
        {
            return {DeviceError(idResult, PresentationStage::Statistics), pbpresenttiming::PresentOutcome::Success, std::nullopt,
                    static_cast<std::int32_t>(idResult)};
        }
        const auto debug = CheckDebugLayer();
        return {debug, pbpresenttiming::PresentOutcome::Success, SUCCEEDED(idResult) ? std::optional<std::uint32_t>{presentId} : std::nullopt,
                static_cast<std::int32_t>(idResult)};
    }

    [[nodiscard]] BackendStatistics GetStatistics() noexcept override
    {
        if (!swapChain_)
        {
            return {};
        }
        DXGI_FRAME_STATISTICS statistics{};
        const HRESULT result = swapChain_->GetFrameStatistics(&statistics);
        if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DEVICE_HUNG)
        {
            return {pbpresenttiming::StatisticsStatus::Error, {}, static_cast<std::int32_t>(result), DeviceError(result, PresentationStage::Statistics)};
        }
        if (result == DXGI_ERROR_FRAME_STATISTICS_DISJOINT)
        {
            return {pbpresenttiming::StatisticsStatus::Disjoint, {}, static_cast<std::int32_t>(result)};
        }
        if (result == DXGI_ERROR_UNSUPPORTED || result == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE || result == E_NOTIMPL)
        {
            return {pbpresenttiming::StatisticsStatus::Unavailable, {}, static_cast<std::int32_t>(result)};
        }
        if (FAILED(result))
        {
            return {pbpresenttiming::StatisticsStatus::Error, {}, static_cast<std::int32_t>(result)};
        }
        return {pbpresenttiming::StatisticsStatus::Valid,
                {statistics.PresentCount, statistics.PresentRefreshCount, statistics.SyncRefreshCount, statistics.SyncQPCTime.QuadPart,
                 statistics.SyncGPUTime.QuadPart},
                0};
    }

    void Wake() noexcept override
    {
        if (wake_.Get() != nullptr)
        {
            SetEvent(wake_.Get());
        }
    }
    void Cancel() noexcept override
    {
        cancelled_.store(true);
        Wake();
    }
    void Shutdown() noexcept override
    {
        ReleaseGraphics();
        factory_.Reset();
        const HWND window = window_.exchange(nullptr);
        if (window != nullptr)
        {
            DestroyWindow(window);
        }
        if (classRegistered_)
        {
            UnregisterClassW(className_.data(), instance_);
            classRegistered_ = false;
        }
        // The owner thread has left every wait before Shutdown is called.
        // Retaining the wake event until backend destruction would leave a
        // stopped DataWindow holding an otherwise unnecessary kernel handle.
        wake_.Reset();
        if (options_.shutdownDiagnostics != nullptr)
        {
            *options_.shutdownDiagnostics = GetDiagnostics();
        }
    }
    [[nodiscard]] std::uintptr_t GetWindowToken() const noexcept override
    {
        return reinterpret_cast<std::uintptr_t>(window_.load());
    }

private:
    [[nodiscard]] static LRESULT CALLBACK WindowProcedure(const HWND window, const UINT message, const WPARAM word, const LPARAM longWord) noexcept
    {
        auto* backend = reinterpret_cast<NativeBackend*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* const creation = reinterpret_cast<const CREATESTRUCTW*>(longWord);
            backend = static_cast<NativeBackend*>(creation->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(backend));
        }
        if (backend != nullptr)
        {
            switch (message)
            {
            case WM_CLOSE:
                backend->closeRequested_ = true;
                return 0;
            case WM_KEYDOWN:
                if (word == VK_ESCAPE)
                {
                    backend->closeRequested_ = true;
                    return 0;
                }
                break;
            case WM_DPICHANGED:
                backend->suggestedDpiRect_ = *reinterpret_cast<const RECT*>(longWord);
                pbprotocol::SaturatingIncrementUnsigned(backend->dpiChangeSerial_);
                return 0;
            case WM_DISPLAYCHANGE:
                pbprotocol::SaturatingIncrementUnsigned(backend->modeChangeSerial_);
                break;
            case WM_ERASEBKGND:
                return 1;
            case WM_NCDESTROY:
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                break;
            default:
                break;
            }
        }
        return DefWindowProcW(window, message, word, longWord);
    }

    void PumpMessages() noexcept
    {
        MSG message{};
        // A hostile message producer cannot starve cancellation or the
        // present queue indefinitely within one pump iteration.
        for (unsigned int index = 0; index < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); index++)
        {
            if (message.message == WM_QUIT)
            {
                closeRequested_ = true;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    [[nodiscard]] bool ShouldFail(const PresentationStage stage) const noexcept
    {
        return options_.failAfterStage == stage;
    }
    [[nodiscard]] static PresentationStatus InjectedFailure(const PresentationStage stage) noexcept
    {
        return NativeError(E_FAIL, stage);
    }

    [[nodiscard]] PresentationStatus RefreshFactory() noexcept
    {
        if (factory_ && factory_->IsCurrent())
        {
            return PresentationStatus::Success();
        }
        ComPtr<IDXGIFactory2> factory;
        const HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
        if (FAILED(result))
        {
            return NativeError(result, PresentationStage::Adapter);
        }
        factory_ = std::move(factory);
        return PresentationStatus::Success();
    }

    [[nodiscard]] bool FindAdapter(const HMONITOR monitor, ComPtr<IDXGIAdapter1>& result, DXGI_ADAPTER_DESC1& description) noexcept
    {
        for (UINT adapterIndex = 0; adapterIndex < 64; adapterIndex++)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory_->EnumAdapters1(adapterIndex, adapter.GetAddressOf()) != S_OK)
            {
                break;
            }
            for (UINT outputIndex = 0; outputIndex < 64; outputIndex++)
            {
                ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(outputIndex, output.GetAddressOf()) != S_OK)
                {
                    break;
                }
                DXGI_OUTPUT_DESC outputDescription{};
                if (SUCCEEDED(output->GetDesc(&outputDescription)) && outputDescription.Monitor == monitor && SUCCEEDED(adapter->GetDesc1(&description)))
                {
                    result = std::move(adapter);
                    return true;
                }
            }
        }
        return false;
    }

    [[nodiscard]] PresentationStatus CreateGraphics(const WindowEnvironment& environment) noexcept
    {
        const std::uint64_t pixels = static_cast<std::uint64_t>(environment.clientWidth) * environment.clientHeight;
        if (environment.clientWidth == 0 || environment.clientHeight == 0 || environment.clientWidth > 16384 || environment.clientHeight > 16384 ||
            pixels > config_.maximumFrameBytes / 4)
        {
            return PresentationStatus::Failure(PresentationErrorCode::ResourceLimit, PresentationStage::BackBuffer);
        }
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 description{};
        if (!FindAdapter(reinterpret_cast<HMONITOR>(static_cast<std::uintptr_t>(environment.monitorIdentity)), adapter, description))
        {
            return PresentationStatus::Failure(PresentationErrorCode::NativeFailure, PresentationStage::Adapter);
        }
        if (!options_.warp && (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            return PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::Adapter);
        }
        const std::array<D3D_FEATURE_LEVEL, 2> featureLevels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | (options_.debugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0u);
        D3D_FEATURE_LEVEL selectedLevel{};
        HRESULT result = D3D11CreateDevice(options_.warp ? nullptr : adapter.Get(), options_.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                           flags, featureLevels.data(), static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION, device_.GetAddressOf(),
                                           &selectedLevel, context_.GetAddressOf());
        if (FAILED(result))
        {
            return NativeError(result, PresentationStage::Device);
        }
        deviceMonitorLuid_ = description.AdapterLuid;
        if (options_.debugLayer)
        {
            result = device_.As(&infoQueue_);
            if (FAILED(result))
            {
                return NativeError(result, PresentationStage::DebugLayer);
            }
            result = infoQueue_->SetMessageCountLimit(128);
            if (FAILED(result))
            {
                return NativeError(result, PresentationStage::DebugLayer);
            }
        }
        if (ShouldFail(PresentationStage::Device))
        {
            return InjectedFailure(PresentationStage::Device);
        }
        D3D11_QUERY_DESC queryDescription{};
        queryDescription.Query = D3D11_QUERY_EVENT;
        result = device_->CreateQuery(&queryDescription, completion_.GetAddressOf());
        if (FAILED(result))
        {
            return NativeError(result, PresentationStage::GpuDrain);
        }
        DXGI_SWAP_CHAIN_DESC1 swapDescription{};
        swapDescription.Width = environment.clientWidth;
        swapDescription.Height = environment.clientHeight;
        swapDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDescription.SampleDesc.Count = 1;
        swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDescription.BufferCount = config_.bufferCount;
        swapDescription.Scaling = DXGI_SCALING_NONE;
        swapDescription.SwapEffect = config_.flipEffect == FlipEffect::Discard ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        swapDescription.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        swapDescription.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> baseSwapChain;
        result = factory_->CreateSwapChainForHwnd(device_.Get(), window_.load(), &swapDescription, nullptr, nullptr, baseSwapChain.GetAddressOf());
        if (FAILED(result))
        {
            return DeviceError(result, PresentationStage::SwapChain);
        }
        result = baseSwapChain.As(&swapChain_);
        if (FAILED(result))
        {
            return NativeError(result, PresentationStage::SwapChain);
        }
        const auto generation = pbprotocol::CheckedAddUnsigned(diagnostics_.swapChainGeneration, std::uint64_t{1});
        if (!generation)
        {
            return PresentationStatus::Failure(PresentationErrorCode::InternalError, PresentationStage::SwapChain);
        }
        diagnostics_.swapChainGeneration = generation.Value();
        result = factory_->MakeWindowAssociation(window_.load(), DXGI_MWA_NO_ALT_ENTER);
        if (FAILED(result))
        {
            return NativeError(result, PresentationStage::SwapChain);
        }
        if (ShouldFail(PresentationStage::SwapChain))
        {
            return InjectedFailure(PresentationStage::SwapChain);
        }
        result = swapChain_->SetMaximumFrameLatency(config_.maximumFrameLatency);
        if (FAILED(result))
        {
            return NativeError(result, PresentationStage::FrameLatency);
        }
        frameLatency_.Reset(swapChain_->GetFrameLatencyWaitableObject());
        if (frameLatency_.Get() == nullptr)
        {
            return PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::FrameLatency);
        }
        if (ShouldFail(PresentationStage::FrameLatency))
        {
            return InjectedFailure(PresentationStage::FrameLatency);
        }
        const auto buffer = CreateBackBuffer();
        return buffer ? RefreshContract() : buffer;
    }

    [[nodiscard]] PresentationStatus CreateBackBuffer() noexcept
    {
        HRESULT result = swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer_.GetAddressOf()));
        if (FAILED(result))
        {
            return DeviceError(result, PresentationStage::BackBuffer);
        }
        const auto generation = pbprotocol::CheckedAddUnsigned(diagnostics_.bufferGeneration, std::uint64_t{1});
        if (!generation)
        {
            return PresentationStatus::Failure(PresentationErrorCode::InternalError, PresentationStage::BackBuffer);
        }
        diagnostics_.bufferGeneration = generation.Value();
        if (options_.verifyUploads)
        {
            D3D11_TEXTURE2D_DESC description{};
            backBuffer_->GetDesc(&description);
            description.Usage = D3D11_USAGE_STAGING;
            description.BindFlags = 0;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            description.MiscFlags = 0;
            result = device_->CreateTexture2D(&description, nullptr, staging_.GetAddressOf());
            if (FAILED(result))
            {
                return NativeError(result, PresentationStage::Readback);
            }
        }
        return ShouldFail(PresentationStage::BackBuffer) ? InjectedFailure(PresentationStage::BackBuffer) : PresentationStatus::Success();
    }

    [[nodiscard]] PresentationStatus RefreshContract() noexcept
    {
        DXGI_SWAP_CHAIN_DESC1 description{};
        UINT latency = 0;
        HRESULT result = swapChain_->GetDesc1(&description);
        if (FAILED(result))
        {
            return NativeError(result, PresentationStage::SwapChain);
        }
        result = swapChain_->GetMaximumFrameLatency(&latency);
        if (FAILED(result))
        {
            return NativeError(result, PresentationStage::FrameLatency);
        }
        contract_.bufferWidth = description.Width;
        contract_.bufferHeight = description.Height;
        contract_.bufferCount = description.BufferCount;
        contract_.maximumFrameLatency = latency;
        contract_.flipEffect = description.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? FlipEffect::Discard : FlipEffect::Sequential;
        contract_.bgraUnorm = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
        contract_.noMsaa = description.SampleDesc.Count == 1 && description.SampleDesc.Quality == 0;
        contract_.alphaIgnored = description.AlphaMode == DXGI_ALPHA_MODE_IGNORE;
        contract_.scalingNone = description.Scaling == DXGI_SCALING_NONE;
        contract_.tearingDisabled = (description.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) == 0;
        contract_.latencyWaitable = (description.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0 && frameLatency_.Get() != nullptr;
        contract_.perMonitorV2 =
            AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(window_.load()), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
        if (description.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD && description.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
        {
            return PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::SwapChain);
        }
        return CheckDebugLayer();
    }

    [[nodiscard]] PresentationStatus DrainGpu() noexcept
    {
        if (!context_ || !completion_)
        {
            return PresentationStatus::Success();
        }
        context_->End(completion_.Get());
        context_->Flush();
        const std::int64_t start = NowQpc();
        for (;;)
        {
            if (cancelled_.load())
            {
                return PresentationStatus::Failure(PresentationErrorCode::NotRunning, PresentationStage::GpuDrain);
            }
            BOOL completed = FALSE;
            const HRESULT result = context_->GetData(completion_.Get(), &completed, sizeof(completed), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (result == S_OK && completed)
            {
                return PresentationStatus::Success();
            }
            if (FAILED(result))
            {
                return DeviceError(result, PresentationStage::GpuDrain);
            }
            const std::int64_t now = NowQpc();
            if (start < 0 || now < start || static_cast<double>(now - start) * 1000.0 / static_cast<double>(frequency_) >= config_.waitTimeoutMilliseconds)
            {
                return PresentationStatus::Failure(PresentationErrorCode::Timeout, PresentationStage::GpuDrain);
            }
            const HANDLE wake = wake_.Get();
            const DWORD wait = MsgWaitForMultipleObjectsEx(1, &wake, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (wait == WAIT_FAILED)
            {
                return PresentationStatus::Failure(PresentationErrorCode::WaitFailed, PresentationStage::GpuDrain, static_cast<std::int32_t>(GetLastError()));
            }
            PumpMessages();
        }
    }

    [[nodiscard]] PresentationStatus DeviceError(const HRESULT result, const PresentationStage stage) const noexcept
    {
        if ((result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) && device_)
        {
            const HRESULT reason = device_->GetDeviceRemovedReason();
            return PresentationStatus::Failure(PresentationErrorCode::DeviceLost, stage, static_cast<std::int32_t>(FAILED(reason) ? reason : result));
        }
        return NativeError(result, stage);
    }

    [[nodiscard]] PresentationStatus CheckDebugLayer() noexcept
    {
        if (!infoQueue_)
        {
            return PresentationStatus::Success();
        }
        if (infoQueue_->GetNumMessagesDiscardedByMessageCountLimit() != 0)
        {
            pbprotocol::SaturatingIncrementUnsigned(diagnostics_.debugErrors);
        }
        const UINT64 count = infoQueue_->GetNumStoredMessages();
        for (UINT64 index = 0; index < count; index++)
        {
            alignas(D3D11_MESSAGE) std::array<std::byte, 4096> storage{};
            SIZE_T bytes = storage.size();
            auto* const message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            const HRESULT result = infoQueue_->GetMessage(index, message, &bytes);
            if (FAILED(result) || message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION || message->Severity == D3D11_MESSAGE_SEVERITY_ERROR)
            {
                pbprotocol::SaturatingIncrementUnsigned(diagnostics_.debugErrors);
            }
        }
        infoQueue_->ClearStoredMessages();
        return diagnostics_.debugErrors == 0 ? PresentationStatus::Success()
                                             : PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::DebugLayer);
    }

    void ReleaseGraphics() noexcept
    {
        if (context_)
        {
            context_->ClearState();
        }
        sourceTexture_.Reset();
        staging_.Reset();
        backBuffer_.Reset();
        completion_.Reset();
        // No wait is active here: all waits and teardown share the owner.
        frameLatency_.Reset();
        swapChain_.Reset();
        if (context_)
        {
            // Flip chains are HWND-exclusive. Flush deferred destruction
            // AFTER releasing the old chain and its buffers, before another
            // CreateSwapChainForHwnd can use this HWND during migration.
            context_->Flush();
        }
        infoQueue_.Reset();
        context_.Reset();
        device_.Reset();
    }

    const NativeBackendTestOptions options_;
    DataWindowConfig config_;
    std::int64_t frequency_ = 0;
    OwnedHandle wake_;
    DWORD wakeError_ = 0;
    OwnedHandle frameLatency_;
    std::atomic<bool> cancelled_ = false;
    std::atomic<HWND> window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    std::array<wchar_t, 96> className_{};
    bool classRegistered_ = false;
    bool closeRequested_ = false;
    bool occluded_ = false;
    std::optional<RECT> suggestedDpiRect_;
    std::uint64_t modeChangeSerial_ = 0;
    std::uint64_t dpiChangeSerial_ = 0;
    LUID deviceMonitorLuid_{};
    ComPtr<IDXGIFactory2> factory_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain2> swapChain_;
    ComPtr<ID3D11Texture2D> backBuffer_;
    ComPtr<ID3D11Texture2D> sourceTexture_;
    ComPtr<ID3D11Texture2D> staging_;
    ComPtr<ID3D11Query> completion_;
    ComPtr<ID3D11InfoQueue> infoQueue_;
    PresentationContract contract_;
    BackendDiagnostics diagnostics_;
};

}

std::unique_ptr<PresentationBackend> MakeNativeBackend(const NativeBackendTestOptions& options)
{
    return std::make_unique<NativeBackend>(options);
}

}
