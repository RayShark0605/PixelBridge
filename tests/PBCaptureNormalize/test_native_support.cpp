#include "native_support.h"

#include <catch2/catch_test_macros.hpp>
#include <wrl/implements.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <limits>
#include <objbase.h>
#include <thread>
#include <type_traits>
#include <vector>

using namespace pbcapturenormalize;
using namespace pbcapturenormalize::detail;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

static_assert(!std::is_copy_constructible_v<DeferredGpuRetirement>);
static_assert(!std::is_copy_assignable_v<DeferredGpuRetirement>);
static_assert(!std::is_move_constructible_v<DeferredGpuRetirement>);
static_assert(!std::is_move_assignable_v<DeferredGpuRetirement>);

namespace
{

template<typename Interface, typename... BaseInterfaces>
class DxgiObjectStub : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                          Microsoft::WRL::ChainInterfaces<Interface, BaseInterfaces...>>
{
public:
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override
    {
        return E_NOTIMPL;
    }
};

class FakeOutput final : public DxgiObjectStub<IDXGIOutput6, IDXGIOutput5, IDXGIOutput4, IDXGIOutput3, IDXGIOutput2, IDXGIOutput1, IDXGIOutput, IDXGIObject>
{
public:
    DXGI_OUTPUT_DESC description{};
    DXGI_OUTPUT_DESC1 description1{};
    HRESULT descriptionError = S_OK;
    HRESULT description1Error = S_OK;
    UINT descriptionReads = 0;
    UINT description1Reads = 0;

    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC* const result) override
    {
        descriptionReads++;
        if (FAILED(descriptionError))
        {
            return descriptionError;
        }
        *result = description;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_OUTPUT_DESC1* const result) override
    {
        description1Reads++;
        if (FAILED(description1Error))
        {
            return description1Error;
        }
        *result = description1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDisplayModeList(DXGI_FORMAT, UINT, UINT*, DXGI_MODE_DESC*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(const DXGI_MODE_DESC*, DXGI_MODE_DESC*, IUnknown*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE WaitForVBlank() override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown*, BOOL) override
    {
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE ReleaseOwnership() override
    {
    }
    HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetDisplayModeList1(DXGI_FORMAT, UINT, UINT*, DXGI_MODE_DESC1*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode1(const DXGI_MODE_DESC1*, DXGI_MODE_DESC1*, IUnknown*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData1(IDXGIResource*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE DuplicateOutput(IUnknown*, IDXGIOutputDuplication**) override
    {
        return E_NOTIMPL;
    }
    BOOL STDMETHODCALLTYPE SupportsOverlays() override
    {
        return FALSE;
    }
    HRESULT STDMETHODCALLTYPE CheckOverlaySupport(DXGI_FORMAT, IUnknown*, UINT*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CheckOverlayColorSpaceSupport(DXGI_FORMAT, DXGI_COLOR_SPACE_TYPE, IUnknown*, UINT*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE DuplicateOutput1(IUnknown*, UINT, UINT, const DXGI_FORMAT*, IDXGIOutputDuplication**) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CheckHardwareCompositionSupport(UINT*) override
    {
        return E_NOTIMPL;
    }
};

class FakeAdapter final : public DxgiObjectStub<IDXGIAdapter1, IDXGIAdapter, IDXGIObject>
{
public:
    std::vector<ComPtr<FakeOutput>> outputs;
    DXGI_ADAPTER_DESC1 description{};
    HRESULT descriptionError = S_OK;
    UINT descriptionReads = 0;
    UINT enumerationErrorIndex = std::numeric_limits<UINT>::max();
    HRESULT enumerationError = E_FAIL;
    std::array<UINT, 66> outputQueries{};
    std::size_t outputQueryCount = 0;

    HRESULT STDMETHODCALLTYPE EnumOutputs(const UINT index, IDXGIOutput** const result) override
    {
        *result = nullptr;
        if (outputQueryCount == outputQueries.size())
        {
            return E_UNEXPECTED;
        }
        outputQueries[outputQueryCount++] = index;
        if (index == enumerationErrorIndex)
        {
            return enumerationError;
        }
        return index < outputs.size() ? outputs[index].CopyTo(result) : DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* const result) override
    {
        descriptionReads++;
        if (FAILED(descriptionError))
        {
            return descriptionError;
        }
        *result = description;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID, LARGE_INTEGER*) override
    {
        return E_NOTIMPL;
    }
};

class FakeFactory final : public DxgiObjectStub<IDXGIFactory1, IDXGIFactory, IDXGIObject>
{
public:
    std::vector<ComPtr<FakeAdapter>> adapters;
    UINT enumerationErrorIndex = std::numeric_limits<UINT>::max();
    HRESULT enumerationError = E_FAIL;
    std::array<UINT, 66> adapterQueries{};
    std::size_t adapterQueryCount = 0;

    HRESULT STDMETHODCALLTYPE EnumAdapters1(const UINT index, IDXGIAdapter1** const result) override
    {
        *result = nullptr;
        if (adapterQueryCount == adapterQueries.size())
        {
            return E_UNEXPECTED;
        }
        adapterQueries[adapterQueryCount++] = index;
        if (index == enumerationErrorIndex)
        {
            return enumerationError;
        }
        return index < adapters.size() ? adapters[index].CopyTo(result) : DXGI_ERROR_NOT_FOUND;
    }
    BOOL STDMETHODCALLTYPE IsCurrent() override
    {
        return TRUE;
    }
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT, IDXGIAdapter**) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND, UINT) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE, IDXGIAdapter**) override
    {
        return E_NOTIMPL;
    }
};

BOOL WINAPI ReadTestDisplayMode(LPCWSTR const name, const DWORD modeNumber, DEVMODEW* const mode, const DWORD flags)
{
    if (name == nullptr || mode == nullptr || mode->dmSize != sizeof(DEVMODEW) || modeNumber != ENUM_CURRENT_SETTINGS || flags != 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (std::wcscmp(name, L"PB_TEST_ERROR5") == 0)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (std::wcscmp(name, L"PB_TEST_ERROR0") == 0)
    {
        SetLastError(ERROR_SUCCESS);
        return FALSE;
    }
    if (std::wcscmp(name, L"PB_TEST_DISPLAY") != 0)
    {
        SetLastError(ERROR_BAD_DEVICE);
        return FALSE;
    }
    mode->dmDisplayFrequency = 144;
    return TRUE;
}

bool SameRectangle(const RECT& left, const RECT& right)
{
    return left.left == right.left && left.top == right.top && left.right == right.right && left.bottom == right.bottom;
}

void RequireSameEnvironment(const CaptureEnvironment& actual, const CaptureEnvironment& expected)
{
    REQUIRE(actual.region.monitor == expected.region.monitor);
    REQUIRE(SameRectangle(actual.region.physicalRect, expected.region.physicalRect));
    REQUIRE(SameRectangle(actual.region.monitorPhysicalRect, expected.region.monitorPhysicalRect));
    REQUIRE(actual.region.dpiX == expected.region.dpiX);
    REQUIRE(actual.region.dpiY == expected.region.dpiY);
    REQUIRE(actual.region.rotation == expected.region.rotation);
    REQUIRE(actual.contentSize.width == expected.contentSize.width);
    REQUIRE(actual.contentSize.height == expected.contentSize.height);
    REQUIRE(actual.pixelFormat == expected.pixelFormat);
    REQUIRE(actual.adapterLuid.LowPart == expected.adapterLuid.LowPart);
    REQUIRE(actual.adapterLuid.HighPart == expected.adapterLuid.HighPart);
    REQUIRE(actual.displayFrequency == expected.displayFrequency);
    REQUIRE(actual.bitsPerColor == expected.bitsPerColor);
    REQUIRE(actual.outputColorSpace == expected.outputColorSpace);
    REQUIRE(actual.hdr == expected.hdr);
    REQUIRE(actual.backendKind == expected.backendKind);
    REQUIRE(actual.sourceSize.width == expected.sourceSize.width);
    REQUIRE(actual.sourceSize.height == expected.sourceSize.height);
    REQUIRE(actual.sourceRotation == expected.sourceRotation);
}

ULONG ReferenceCount(IUnknown* const object)
{
    object->AddRef();
    return object->Release();
}

pbscreenregion::ScreenCaptureRegion MakeRegion()
{
    return {reinterpret_cast<HMONITOR>(std::uintptr_t{17}), {-310, -201, -101, 307}, {-900, -300, 300, 1600}, 144, 192, DXGI_MODE_ROTATION_ROTATE270};
}

CaptureEnvironment MakeEnvironment()
{
    CaptureEnvironment environment;
    environment.region = MakeRegion();
    environment.contentSize = {1200, 1900};
    environment.pixelFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    environment.adapterLuid = {0x1234abcd, -71};
    environment.displayFrequency = 75;
    environment.bitsPerColor = 8;
    environment.outputColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    environment.hdr = false;
    environment.backendKind = CaptureBackendKind::Dxgi;
    environment.sourceSize = {1900, 1200};
    environment.sourceRotation = DXGI_MODE_ROTATION_ROTATE270;
    return environment;
}

ComPtr<FakeOutput> MakeOutput(const bool matchesRegion)
{
    auto output = Make<FakeOutput>();
    REQUIRE(output);
    const auto region = MakeRegion();
    output->description.Monitor = matchesRegion ? region.monitor : reinterpret_cast<HMONITOR>(std::uintptr_t{99});
    output->description.AttachedToDesktop = TRUE;
    output->description.DesktopCoordinates = region.monitorPhysicalRect;
    output->description.Rotation = region.rotation;
    REQUIRE(wcscpy_s(output->description.DeviceName, L"PB_TEST_DISPLAY") == 0);
    REQUIRE(wcscpy_s(output->description1.DeviceName, L"PB_TEST_DISPLAY") == 0);
    output->description1.BitsPerColor = 10;
    output->description1.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    return output;
}

struct Topology
{
    ComPtr<FakeFactory> factory;
    ComPtr<FakeAdapter> selectedAdapter;
    ComPtr<FakeOutput> selectedOutput;
};

Topology MakeTopology(const UINT adapterCount, const UINT outputCount, const UINT targetAdapter = 0, const UINT targetOutput = 0)
{
    Topology topology;
    topology.factory = Make<FakeFactory>();
    REQUIRE(topology.factory);
    for (UINT adapterIndex = 0; adapterIndex < adapterCount; adapterIndex++)
    {
        auto adapter = Make<FakeAdapter>();
        REQUIRE(adapter);
        adapter->description.AdapterLuid = {adapterIndex + 101, 72};
        if (adapterIndex == targetAdapter)
        {
            topology.selectedAdapter = adapter;
            for (UINT outputIndex = 0; outputIndex < outputCount; outputIndex++)
            {
                auto output = MakeOutput(outputIndex == targetOutput);
                if (outputIndex == targetOutput)
                {
                    topology.selectedOutput = output;
                }
                adapter->outputs.push_back(std::move(output));
            }
        }
        topology.factory->adapters.push_back(std::move(adapter));
    }
    REQUIRE(topology.selectedAdapter);
    REQUIRE(topology.selectedOutput);
    return topology;
}

void CheckFind(const Topology& topology, const CaptureError expectedError, const HRESULT expectedNativeError = S_OK)
{
    const auto originalAdapter = Make<FakeAdapter>();
    const auto originalOutput = MakeOutput(false);
    REQUIRE(originalAdapter);
    ComPtr<IDXGIAdapter1> adapter = originalAdapter;
    ComPtr<IDXGIOutput6> output = originalOutput;
    auto environment = MakeEnvironment();
    const auto before = environment;
    const ULONG adapterReferences = ReferenceCount(topology.selectedAdapter.Get());
    const ULONG outputReferences = ReferenceCount(topology.selectedOutput.Get());
    const auto status = FindCaptureAdapter(topology.factory.Get(), MakeRegion(), adapter, output, environment, ReadTestDisplayMode);
    REQUIRE(status.code == expectedError);
    REQUIRE(status.nativeError == expectedNativeError);
    if (expectedError != CaptureError::None)
    {
        REQUIRE(status.stage == CaptureStage::Adapter);
        REQUIRE(adapter.Get() == originalAdapter.Get());
        REQUIRE(output.Get() == originalOutput.Get());
        RequireSameEnvironment(environment, before);
        REQUIRE(ReferenceCount(topology.selectedAdapter.Get()) == adapterReferences);
        REQUIRE(ReferenceCount(topology.selectedOutput.Get()) == outputReferences);
        return;
    }
    REQUIRE(adapter.Get() == topology.selectedAdapter.Get());
    REQUIRE(output.Get() == topology.selectedOutput.Get());
    auto expected = before;
    expected.adapterLuid = topology.selectedAdapter->description.AdapterLuid;
    expected.displayFrequency = 144;
    expected.bitsPerColor = 10;
    expected.outputColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    expected.hdr = true;
    RequireSameEnvironment(environment, expected);
    REQUIRE(ReferenceCount(topology.selectedAdapter.Get()) == adapterReferences + 1);
    REQUIRE(ReferenceCount(topology.selectedOutput.Get()) == outputReferences + 1);
}

void CheckQueries(const std::array<UINT, 66>& queries, const std::size_t queryCount, const UINT itemCount)
{
    const auto expectedCount = (std::min)(itemCount + 1u, 65u);
    REQUIRE(queryCount == expectedCount);
    for (UINT index = 0; index < expectedCount; index++)
    {
        REQUIRE(queries[index] == index);
    }
}

struct WarpDevice
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

WarpDevice MakeWarpDevice()
{
    WarpDevice native;
    constexpr std::array levels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL actual{};
    REQUIRE(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels.data(),
                             static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &native.device, &actual, &native.context) == S_OK);
    return native;
}

struct RetirementState
{
    std::atomic<UINT> completions{0};
    std::atomic<UINT> destructions{0};
    HRESULT apartmentResult = E_FAIL;
    APTTYPE apartmentType = APTTYPE_CURRENT;
};

class TestEvent
{
public:
    explicit TestEvent(const bool signaled) : handle_(CreateEventW(nullptr, TRUE, signaled ? TRUE : FALSE, nullptr))
    {
        REQUIRE(handle_ != nullptr);
    }
    ~TestEvent()
    {
        CloseHandle(handle_);
    }
    TestEvent(const TestEvent&) = delete;
    TestEvent& operator=(const TestEvent&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept
    {
        return handle_;
    }

private:
    const HANDLE handle_;
};

struct ProducerSignalOnExit
{
    std::shared_ptr<TestEvent> event;
    ~ProducerSignalOnExit()
    {
        SetEvent(event->Get());
    }
};

class RetirementOwner final : public DeferredCleanup
{
public:
    RetirementOwner(std::shared_ptr<RetirementState> state, std::shared_ptr<TestEvent> producerIdle)
        : state_(std::move(state)), producerIdle_(std::move(producerIdle))
    {
    }
    ~RetirementOwner() override
    {
        state_->destructions.fetch_add(1);
    }
    void CompleteDeferredShutdown() noexcept override
    {
        APTTYPEQUALIFIER qualifier{};
        state_->apartmentResult = CoGetApartmentType(&state_->apartmentType, &qualifier);
        retirement.Reset();
        state_->completions.fetch_add(1);
    }
    DeferredGpuRetirement retirement;

private:
    std::shared_ptr<RetirementState> state_;
    // Also protect error unwinding: a failed assertion signals the producer but
    // cannot close its HANDLE while the retained owner's wait is still pending.
    std::shared_ptr<TestEvent> producerIdle_;
};

}

TEST_CASE("Capture adapter resolution accepts 64 adapters and rejects a real 65th adapter")
{
    for (const UINT adapterCount : {63u, 64u, 65u})
    {
        for (const UINT targetAdapter : {0u, adapterCount - 1})
        {
            CAPTURE(adapterCount, targetAdapter);
            const auto topology = MakeTopology(adapterCount, 1, targetAdapter);
            CheckFind(topology, adapterCount <= 64 ? CaptureError::None : CaptureError::ResourceLimit);
            CheckQueries(topology.factory->adapterQueries, topology.factory->adapterQueryCount, adapterCount);
            if (adapterCount == 65)
            {
                REQUIRE(topology.factory->adapters[64]->outputQueryCount == 0);
                REQUIRE(topology.factory->adapters[64]->descriptionReads == 0);
            }
        }
    }
}

TEST_CASE("Capture adapter resolution accepts 64 outputs and rejects a real 65th output")
{
    for (const UINT outputCount : {63u, 64u, 65u})
    {
        for (const UINT targetOutput : {0u, outputCount - 1})
        {
            CAPTURE(outputCount, targetOutput);
            const auto topology = MakeTopology(1, outputCount, 0, targetOutput);
            CheckFind(topology, outputCount <= 64 ? CaptureError::None : CaptureError::ResourceLimit);
            CheckQueries(topology.selectedAdapter->outputQueries, topology.selectedAdapter->outputQueryCount, outputCount);
            if (outputCount == 65)
            {
                REQUIRE(topology.selectedAdapter->outputs[64]->descriptionReads == 0);
                REQUIRE(topology.selectedAdapter->outputs[64]->description1Reads == 0);
            }
        }
    }
}

TEST_CASE("Capture adapter resolution completes bounded enumeration after finding its target")
{
    for (const UINT laterOutputCount : {63u, 64u, 65u})
    {
        CAPTURE(laterOutputCount);
        const auto topology = MakeTopology(2, 1);
        const auto laterAdapter = topology.factory->adapters[1];
        for (UINT index = 0; index < laterOutputCount; index++)
        {
            laterAdapter->outputs.push_back(MakeOutput(false));
        }
        CheckFind(topology, laterOutputCount <= 64 ? CaptureError::None : CaptureError::ResourceLimit);
        CheckQueries(laterAdapter->outputQueries, laterAdapter->outputQueryCount, laterOutputCount);
    }
}

TEST_CASE("Capture adapter resolution preserves native errors returned by the 64-entry sentinel")
{
    SECTION("adapter sentinel")
    {
        const auto topology = MakeTopology(64, 1);
        topology.factory->enumerationErrorIndex = 64;
        topology.factory->enumerationError = E_OUTOFMEMORY;
        CheckFind(topology, CaptureError::OutOfMemory, E_OUTOFMEMORY);
        CheckQueries(topology.factory->adapterQueries, topology.factory->adapterQueryCount, 64);
    }
    SECTION("output sentinel")
    {
        const auto topology = MakeTopology(1, 64);
        topology.selectedAdapter->enumerationErrorIndex = 64;
        topology.selectedAdapter->enumerationError = E_ACCESSDENIED;
        CheckFind(topology, CaptureError::AccessLost, E_ACCESSDENIED);
        CheckQueries(topology.selectedAdapter->outputQueries, topology.selectedAdapter->outputQueryCount, 64);
    }
}

TEST_CASE("Capture adapter resolution preserves outputs and temporary COM references on every failure")
{
    const auto topology = MakeTopology(1, 1);
    CaptureError expectedError = CaptureError::RegionChanged;
    HRESULT expectedNativeError = S_OK;
    SECTION("adapter sentinel failure after a valid match")
    {
        topology.factory->enumerationErrorIndex = 1;
        topology.factory->enumerationError = E_OUTOFMEMORY;
        expectedError = CaptureError::OutOfMemory;
        expectedNativeError = E_OUTOFMEMORY;
    }
    SECTION("output sentinel failure after a valid match")
    {
        topology.selectedAdapter->enumerationErrorIndex = 1;
        topology.selectedAdapter->enumerationError = E_ACCESSDENIED;
        expectedError = CaptureError::AccessLost;
        expectedNativeError = E_ACCESSDENIED;
    }
    SECTION("unrelated later output descriptor failure")
    {
        const auto laterOutput = MakeOutput(false);
        laterOutput->descriptionError = E_FAIL;
        topology.selectedAdapter->outputs.push_back(laterOutput);
        expectedError = CaptureError::NativeFailure;
        expectedNativeError = E_FAIL;
    }
    SECTION("adapter descriptor failure")
    {
        topology.selectedAdapter->descriptionError = E_NOINTERFACE;
        expectedError = CaptureError::NativeFailure;
        expectedNativeError = E_NOINTERFACE;
    }
    SECTION("extended output descriptor failure")
    {
        topology.selectedOutput->description1Error = E_FAIL;
        expectedError = CaptureError::NativeFailure;
        expectedNativeError = E_FAIL;
    }
    SECTION("display mode access failure")
    {
        REQUIRE(wcscpy_s(topology.selectedOutput->description1.DeviceName, L"PB_TEST_ERROR5") == 0);
        expectedError = CaptureError::AccessLost;
        expectedNativeError = E_ACCESSDENIED;
    }
    SECTION("display mode false cannot be converted to S_OK")
    {
        REQUIRE(wcscpy_s(topology.selectedOutput->description1.DeviceName, L"PB_TEST_ERROR0") == 0);
        expectedError = CaptureError::NativeFailure;
        expectedNativeError = E_FAIL;
    }
    SECTION("duplicate output in the same adapter")
    {
        topology.selectedAdapter->outputs.push_back(topology.selectedOutput);
    }
    SECTION("duplicate output in a second adapter")
    {
        const auto secondAdapter = Make<FakeAdapter>();
        REQUIRE(secondAdapter);
        secondAdapter->outputs.push_back(MakeOutput(true));
        topology.factory->adapters.push_back(secondAdapter);
    }
    SECTION("physical monitor geometry changed")
    {
        topology.selectedOutput->description.DesktopCoordinates.right--;
    }
    SECTION("monitor rotation changed")
    {
        topology.selectedOutput->description.Rotation = DXGI_MODE_ROTATION_ROTATE90;
    }
    SECTION("software adapter cannot silently replace the selected hardware")
    {
        topology.selectedAdapter->description.Flags = DXGI_ADAPTER_FLAG_SOFTWARE;
        expectedError = CaptureError::Unsupported;
    }
    SECTION("selected output is detached")
    {
        topology.selectedOutput->description.AttachedToDesktop = FALSE;
    }
    CheckFind(topology, expectedError, expectedNativeError);
}

TEST_CASE("Capture adapter resolution rejects incomplete arguments without probing or changing output")
{
    const auto topology = MakeTopology(1, 1);
    ComPtr<IDXGIAdapter1> adapter = topology.selectedAdapter;
    ComPtr<IDXGIOutput6> output = topology.selectedOutput;
    auto environment = MakeEnvironment();
    const auto before = environment;
    auto region = MakeRegion();
    IDXGIFactory1* factory = topology.factory.Get();
    DisplayModeReader readDisplayMode = ReadTestDisplayMode;
    SECTION("no factory")
    {
        factory = nullptr;
    }
    SECTION("no monitor")
    {
        region.monitor = nullptr;
    }
    SECTION("no display mode reader")
    {
        readDisplayMode = nullptr;
    }
    const auto status = FindCaptureAdapter(factory, region, adapter, output, environment, readDisplayMode);
    REQUIRE(status.code == CaptureError::InvalidConfiguration);
    REQUIRE(status.stage == CaptureStage::Adapter);
    REQUIRE(status.nativeError == S_OK);
    REQUIRE(adapter.Get() == topology.selectedAdapter.Get());
    REQUIRE(output.Get() == topology.selectedOutput.Get());
    RequireSameEnvironment(environment, before);
    REQUIRE(topology.factory->adapterQueryCount == 0);
}

TEST_CASE("Output state extraction preserves depth and color metadata without tone mapping")
{
    for (const auto colorSpace : {DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
                                 DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709})
    {
        for (const UINT bitsPerColor : {8u, 10u, 16u})
        {
            CAPTURE(colorSpace, bitsPerColor);
            const auto output = MakeOutput(true);
            output->description1.ColorSpace = colorSpace;
            output->description1.BitsPerColor = bitsPerColor;
            auto environment = MakeEnvironment();
            auto expected = environment;
            expected.outputColorSpace = static_cast<std::uint32_t>(colorSpace);
            expected.bitsPerColor = bitsPerColor;
            expected.displayFrequency = 144;
            expected.hdr = colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            REQUIRE(ReadOutputState(*output.Get(), environment, ReadTestDisplayMode) == S_OK);
            RequireSameEnvironment(environment, expected);
            REQUIRE(output->description1Reads == 1);
        }
    }
}

TEST_CASE("Output state extraction commits nothing until descriptor and mode queries both succeed")
{
    const auto output = MakeOutput(true);
    auto environment = MakeEnvironment();
    const auto before = environment;
    HRESULT expectedError = E_FAIL;
    DisplayModeReader readDisplayMode = ReadTestDisplayMode;
    SECTION("descriptor native error")
    {
        output->description1Error = E_OUTOFMEMORY;
        expectedError = E_OUTOFMEMORY;
    }
    SECTION("mode error")
    {
        REQUIRE(wcscpy_s(output->description1.DeviceName, L"PB_TEST_ERROR5") == 0);
        expectedError = E_ACCESSDENIED;
    }
    SECTION("mode failure with zero last error")
    {
        REQUIRE(wcscpy_s(output->description1.DeviceName, L"PB_TEST_ERROR0") == 0);
    }
    SECTION("null mode reader")
    {
        readDisplayMode = nullptr;
        expectedError = E_INVALIDARG;
    }
    REQUIRE(ReadOutputState(*output.Get(), environment, readDisplayMode) == expectedError);
    RequireSameEnvironment(environment, before);
    REQUIRE(output->description1Reads == (readDisplayMode != nullptr ? 1u : 0u));
}

TEST_CASE("Uninitialized deferred retirement has idempotent cleanup and rejects missing native objects")
{
    DeferredGpuRetirement retirement;
    retirement.Reset();
    const auto status = retirement.Initialize(nullptr, nullptr);
    REQUIRE(status.code == CaptureError::InvalidConfiguration);
    REQUIRE(status.stage == CaptureStage::Device);
    REQUIRE(status.nativeError == S_OK);
    retirement.Reset();
    retirement.Reset();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const auto deviceStatus = CreateCaptureDevice(nullptr, false, device, context);
    REQUIRE(deviceStatus.code == CaptureError::InvalidConfiguration);
    REQUIRE(deviceStatus.stage == CaptureStage::Adapter);
    REQUIRE(deviceStatus.nativeError == S_OK);
    REQUIRE_FALSE(device);
    REQUIRE_FALSE(context);
}

TEST_CASE("Deferred retirement rejects foreign and deferred contexts and supports unarmed reset and reinitialization")
{
    const auto first = MakeWarpDevice();
    const auto second = MakeWarpDevice();
    ComPtr<ID3D11DeviceContext> deferredContext;
    REQUIRE(first.device->CreateDeferredContext(0, &deferredContext) == S_OK);
    DeferredGpuRetirement retirement;
    REQUIRE(retirement.Initialize(first.device.Get(), first.context.Get()));
    for (const auto context : {static_cast<ID3D11DeviceContext*>(nullptr), second.context.Get(), deferredContext.Get()})
    {
        const auto status = retirement.Initialize(first.device.Get(), context);
        REQUIRE(status.code == CaptureError::InvalidConfiguration);
        REQUIRE(status.stage == CaptureStage::Device);
        REQUIRE(status.nativeError == S_OK);
    }
    const auto missingDevice = retirement.Initialize(nullptr, first.context.Get());
    REQUIRE(missingDevice.code == CaptureError::InvalidConfiguration);
    REQUIRE(missingDevice.stage == CaptureStage::Device);
    retirement.Reset();
    retirement.Reset();
    REQUIRE(retirement.Initialize(second.device.Get(), second.context.Get()));
    retirement.Reset();
}

TEST_CASE("Deferred retirement retains its owner until GPU and producer completion and cleans up in MTA")
{
    for (const bool producerInitiallyIdle : {false, true})
    {
        CAPTURE(producerInitiallyIdle);
        const auto native = MakeWarpDevice();
        const auto state = std::make_shared<RetirementState>();
        const auto producerIdle = std::make_shared<TestEvent>(producerInitiallyIdle);
        const ProducerSignalOnExit signalOnExit{producerIdle};
        auto owner = std::make_shared<RetirementOwner>(state, producerIdle);
        const std::weak_ptr<RetirementOwner> weakOwner = owner;
        REQUIRE(owner->retirement.Initialize(native.device.Get(), native.context.Get()));
        auto* const retirement = &owner->retirement;
        // The only remaining strong owner is transferred into the real helper.
        // A signaled producer allows completion to destroy the helper immediately.
        retirement->Defer(std::move(owner), producerIdle->Get());
        REQUIRE_FALSE(owner);
        if (!producerInitiallyIdle)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            REQUIRE(state->completions == 0);
            REQUIRE(state->destructions == 0);
            REQUIRE_FALSE(weakOwner.expired());
            REQUIRE(SetEvent(producerIdle->Get()));
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (state->destructions == 0 && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(state->completions == 1);
        REQUIRE(state->destructions == 1);
        REQUIRE(weakOwner.expired());
        REQUIRE(state->apartmentResult == S_OK);
        REQUIRE(state->apartmentType == APTTYPE_MTA);
    }
}
