#include "support.h"
#include "local_desktop_runtime.h"
#include "pbprotocol/blake3_digest.h"
#include "run_report.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace pbstep3a
{
void Require(const bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::string Digest(const std::span<const std::byte> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    const auto digest = pbprotocol::ComputeBlake3Digest(bytes);
    std::string text;
    text.reserve(digest.size() * 2);
    for (const auto item : digest)
    {
        const auto value = std::to_integer<unsigned>(item);
        text += digits[value >> 4];
        text += digits[value & 15];
    }
    return text;
}

std::string JsonString(const std::string& value)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string text = "\"";
    for (const unsigned char character : value)
    {
        if (character == '"' || character == '\\')
        {
            text += '\\';
            text += static_cast<char>(character);
        }
        else if (character < 32)
        {
            text += "\\u00";
            text += digits[character >> 4];
            text += digits[character & 15];
        }
        else
        {
            text += static_cast<char>(character);
        }
    }
    return text + '"';
}

Handle::~Handle()
{
    if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(value_);
    }
}

void RequireLocalPath(const std::filesystem::path& path)
{
    Require(path.is_absolute() && path.has_root_name() && path.root_name().wstring().size() == 2 &&
        GetDriveTypeW(path.root_path().c_str()) == DRIVE_FIXED, "Only explicit local fixed-drive paths are allowed");
}

void WriteNew(const std::filesystem::path& path, const std::span<const std::byte> bytes)
{
    Require(bytes.size() <= pbmodulation::kUnifiedFrameBgraBytes, "Create-only artifact size limit");
    const Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    Require(file.Get() != INVALID_HANDLE_VALUE, "Cannot create new artifact");
    DWORD written = 0;
    Require(WriteFile(file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() &&
        FlushFileBuffers(file.Get()), "Cannot finish new artifact");
}

void WriteNewText(const std::filesystem::path& path, const std::string& text)
{
    WriteNew(path, std::as_bytes(std::span(text)));
}

std::filesystem::path FramePath(const std::filesystem::path& root, const std::uint32_t index)
{
    Require(index < kFixtureFrames, "Fixture frame index outside fixed budget");
    return root / ("frame-" + std::to_string(index) + ".bgra");
}

namespace
{
HANDLE OpenLocalRaster(const std::filesystem::path& path)
{
    RequireLocalPath(path);
    return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}
}

LockedRaster::LockedRaster(const std::filesystem::path& path) : handle_(OpenLocalRaster(path))
{
    Require(handle_.Get() != INVALID_HANDLE_VALUE, "Cannot open frozen raster");
    LARGE_INTEGER size{};
    Require(GetFileSizeEx(handle_.Get(), &size) && size.QuadPart == static_cast<LONGLONG>(pbmodulation::kUnifiedFrameBgraBytes), "Frozen raster must be exactly one canonical BGRA8 frame");
}

std::vector<std::byte> LockedRaster::Read()
{
    LARGE_INTEGER origin{};
    Require(SetFilePointerEx(handle_.Get(), origin, nullptr, FILE_BEGIN), "Cannot rewind frozen raster");
    std::vector<std::byte> pixels(pbmodulation::kUnifiedFrameBgraBytes);
    DWORD count = 0;
    Require(ReadFile(handle_.Get(), pixels.data(), static_cast<DWORD>(pixels.size()), &count, nullptr) && count == pixels.size(), "Frozen raster read truncated");
    return pixels;
}

ProcessBudget::ProcessBudget() : job_(CreateJobObjectW(nullptr, nullptr))
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.ProcessMemoryLimit = 2ULL * 1024 * 1024 * 1024;
    Require(job_.Get() != nullptr && SetInformationJobObject(job_.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
        AssignProcessToJobObject(job_.Get(), GetCurrentProcess()), "Cannot establish mandatory 2 GiB process budget");
}

namespace
{
struct ProducerState
{
    std::filesystem::path root;
    std::array<std::string, kFixtureFrames> frameDigests;
    std::atomic<std::uint32_t> frames = 0;
    std::atomic<bool> stopped = false;
};

class FrozenPresentation final : public pbapp::EncoderPresentation
{
public:
    explicit FrozenPresentation(std::shared_ptr<ProducerState> state) : state_(std::move(state))
    {
    }
    pbrenderd3d::DataWindowSnapshot GetSnapshot() const override
    {
        pbrenderd3d::DataWindowSnapshot snapshot;
        snapshot.state = state_->stopped ? pbrenderd3d::WindowState::Stopped : pbrenderd3d::WindowState::Running;
        snapshot.environment.clientWidth = 1920;
        snapshot.environment.clientHeight = 1080;
        snapshot.contract = {1920, 1080, 2, 1, pbrenderd3d::FlipEffect::Discard, true, true, true, true, true, true, true, true, true, true, true, true};
        snapshot.candidateContractSatisfied = true;
        snapshot.viewport.disposition = pbrenderd3d::PresentationViewportDisposition::Active;
        snapshot.timing.presentationEpoch = 1;
        snapshot.pendingFrame = !state_->stopped && state_->frames >= kFixtureFrames;
        return snapshot;
    }
    pbrenderd3d::PresentationStatus SubmitFrame(const pbrenderd3d::CanonicalBgraFrameView& frame) override
    {
        const auto index = state_->frames.load();
        Require(index < kFixtureFrames && frame.pixels.size() == pbmodulation::kUnifiedFrameBgraBytes, "Production fixture exceeded fixed frame budget");
        WriteNew(FramePath(state_->root, index), frame.pixels);
        state_->frameDigests[index] = Digest(frame.pixels);
        state_->frames++;
        return {};
    }
    void RequestStop() noexcept override
    {
        state_->stopped = true;
    }
    void Stop() noexcept override
    {
        RequestStop();
    }
private:
    std::shared_ptr<ProducerState> state_;
};
}

void MakeFixture(const std::filesystem::path& root)
{
    RequireLocalPath(root);
    Require(std::filesystem::create_directory(root), "Fixture root must be new");
    std::array<std::byte, kSourceBytes> bytes{};
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        bytes[index] = static_cast<std::byte>((index * 73 + 19) & 255);
    }
    const auto source = root / "step3a-source.bin";
    WriteNew(source, bytes);
    const auto state = std::make_shared<ProducerState>();
    state->root = root;
    pbapp::EncoderRuntime encoder([state](const pbrenderd3d::DataWindowConfig&) { return std::make_unique<FrozenPresentation>(state); });
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 15);
    config.sessionStateRoot = root / "encoder-state";
    config.diagnostics = std::make_shared<pbcore::StageDiagnostics>();
    const auto status = encoder.Start(config);
    Require(static_cast<bool>(status), status.message);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (state->frames < kFixtureFrames && encoder.GetSnapshot().state != pbapp::EncoderState::Failed && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    encoder.Stop();
    const auto snapshot = encoder.GetSnapshot();
    Require(state->frames == kFixtureFrames && snapshot.state == pbapp::EncoderState::Stopped, "Production fixture incomplete: " + snapshot.errorDetail);
    std::ostringstream manifest;
    manifest << "{\"schema\":\"PixelBridge.Step3A.FrozenPixels.1\",\"producer\":\"ProductionEncoderRuntime\",\"sessionIdentity\":\"OS-CSPRNG-frozen-in-pixels\","
        << "\"sourceBytes\":" << bytes.size() << ",\"sourceBlake3\":\"" << Digest(bytes) << "\",\"width\":1920,\"height\":1080,\"rowPitch\":7680,"
        << "\"pixelFormat\":\"BGRA8\",\"colorContract\":\"CanonicalSDR_RGB_full_no_conversion\",\"codec\":null,\"frameCount\":" << kFixtureFrames << ",\"frames\":[";
    for (std::uint32_t index = 0; index < kFixtureFrames; index++)
    {
        manifest << (index == 0 ? "" : ",") << "{\"index\":" << index << ",\"file\":\"frame-" << index << ".bgra\",\"blake3\":\"" << state->frameDigests[index] << "\"}";
    }
    manifest << "]}\n";
    WriteNewText(root / "fixture.json", manifest.str());
    WriteNewText(root / "sender-report.json", pbapp::BuildEncoderRunReportJson({"Step3AProducer", "sealed-Step2", "unsealed-tool", "unknown"}, snapshot));
}
}
