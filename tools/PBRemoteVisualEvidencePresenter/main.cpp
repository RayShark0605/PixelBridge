#include "pbdesktoplevels/reference_channel.h"
#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/local_desktop_bootstrap.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbmodulation/shape_chroma.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbrenderd3d/data_window.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

enum class Mode
{
    Describe,
    Present
};

enum class ProfileKind
{
    Direct,
    Shape,
    LowFps
};

struct ProfileBinding
{
    ProfileKind kind = ProfileKind::Direct;
    const char* name = nullptr;
    std::uint64_t visualProfileId = 0;
    std::uint8_t layoutVersion = 0;
    std::uint32_t dataBytes = 0;
};

struct Options
{
    Mode mode = Mode::Describe;
    ProfileKind profile = ProfileKind::Direct;
    pbrenderd3d::PhysicalPoint origin;
    std::uint32_t seconds = 0;
    bool profileSpecified = false;
    bool originSpecified = false;
    bool secondsSpecified = false;
};

[[nodiscard]] bool ParseSigned(const std::string_view text, std::int32_t& output) noexcept
{
    if (text.empty())
    {
        return false;
    }
    const bool negative = text.front() == '-';
    if (negative && text.size() == 1)
    {
        return false;
    }
    const std::uint64_t maximumMagnitude = negative ? 2147483648ULL : 2147483647ULL;
    std::uint64_t magnitude = 0;
    for (std::size_t index = negative ? 1 : 0; index < text.size(); index++)
    {
        const char character = text[index];
        if (character < '0' || character > '9')
        {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (magnitude > (maximumMagnitude - digit) / 10ULL)
        {
            return false;
        }
        magnitude = magnitude * 10ULL + digit;
    }
    output = negative ? magnitude == 2147483648ULL ? (std::numeric_limits<std::int32_t>::min)() :
        -static_cast<std::int32_t>(magnitude) : static_cast<std::int32_t>(magnitude);
    return true;
}

[[nodiscard]] bool ParseUnsigned(const std::string_view text, std::uint32_t& output) noexcept
{
    if (text.empty())
    {
        return false;
    }
    std::uint64_t value = 0;
    for (const char character : text)
    {
        if (character < '0' || character > '9')
        {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (value > ((std::numeric_limits<std::uint32_t>::max)() - digit) / 10ULL)
        {
            return false;
        }
        value = value * 10ULL + digit;
    }
    output = static_cast<std::uint32_t>(value);
    return true;
}

[[nodiscard]] bool ParseOptions(const int argumentCount, const char* const arguments[], Options& output)
{
    if (argumentCount < 2)
    {
        return false;
    }
    Options options;
    const std::string_view mode(arguments[1]);
    if (mode == "describe")
    {
        options.mode = Mode::Describe;
    }
    else if (mode == "present")
    {
        options.mode = Mode::Present;
    }
    else
    {
        return false;
    }
    for (int index = 2; index < argumentCount; index++)
    {
        const std::string_view option(arguments[index]);
        const auto nextArgument = [&]() -> const char*
        {
            index++;
            return index < argumentCount ? arguments[index] : nullptr;
        };
        if (option == "--profile")
        {
            const char* const value = nextArgument();
            if (value == nullptr || options.profileSpecified)
            {
                return false;
            }
            const std::string_view profile(value);
            if (profile == "direct")
            {
                options.profile = ProfileKind::Direct;
            }
            else if (profile == "shape")
            {
                options.profile = ProfileKind::Shape;
            }
            else if (profile == "lf4")
            {
                options.profile = ProfileKind::LowFps;
            }
            else
            {
                return false;
            }
            options.profileSpecified = true;
        }
        else if (option == "--origin")
        {
            const char* const x = nextArgument();
            const char* const y = nextArgument();
            if (x == nullptr || y == nullptr || options.originSpecified ||
                !ParseSigned(x, options.origin.x) || !ParseSigned(y, options.origin.y))
            {
                return false;
            }
            options.originSpecified = true;
        }
        else if (option == "--seconds")
        {
            const char* const value = nextArgument();
            if (value == nullptr || options.secondsSpecified || !ParseUnsigned(value, options.seconds) ||
                options.seconds == 0 || options.seconds > 600)
            {
                return false;
            }
            options.secondsSpecified = true;
        }
        else
        {
            return false;
        }
    }
    if (!options.profileSpecified ||
        (options.mode == Mode::Describe && (options.originSpecified || options.secondsSpecified)) ||
        (options.mode == Mode::Present && (!options.originSpecified || !options.secondsSpecified)))
    {
        return false;
    }
    output = options;
    return true;
}

[[nodiscard]] ProfileBinding GetProfile(const ProfileKind kind)
{
    if (kind == ProfileKind::Shape)
    {
        return {kind, "PB-Mod-ShapeChroma-1", pbmodulation::kShapeChromaProfileId,
            pbmodulation::kShapeChromaLayoutVersion, pbmodulation::kShapeChromaDataBytes};
    }
    if (kind == ProfileKind::LowFps)
    {
        return {kind, "PB-RemoteVisual-LF4-X1", pbmodulation::kRemoteVisualLowFpsProfileId,
            pbmodulation::kRemoteVisualLowFpsLayoutVersion, pbmodulation::kRemoteVisualLowFpsDataBytes};
    }
    const auto* const profile = pbmodulation::GetDesktopLevelsProfile(pbmodulation::kDesktopLevels2ProfileId);
    if (profile == nullptr)
    {
        throw std::runtime_error("Direct-Level 2x2 profile is unavailable");
    }
    return {kind, "PB-Mod-DesktopLevels-2x2", profile->visualProfileId,
        pbmodulation::kDesktopLevelsLayoutVersion, profile->dataBytes};
}

[[nodiscard]] std::vector<std::byte> BuildRaster(const ProfileBinding& profile)
{
    pbprotocol::BootstrapRecord record;
    record.protocolVersion = pbprotocol::GetProtocolVersion();
    record.visualLayoutVersion = profile.layoutVersion;
    record.visualProfileId = profile.visualProfileId;
    record.sessionTag.value = 0x5354455030325244ULL;
    record.frameSequence = 17;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bootstrap{};
    if (!pbprotocol::SerializeBootstrapRecord(record, bootstrap))
    {
        throw std::runtime_error("Bootstrap serialization failed");
    }
    std::vector<std::byte> data(profile.dataBytes);
    if (!pbdesktoplevels::GenerateDiagnosticData(bootstrap, data))
    {
        throw std::runtime_error("diagnostic Transport generation failed");
    }
    std::vector<std::byte> raster(static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) *
        pbmodulation::kLocalDesktopCanvasHeight * 4);
    const bool encoded = profile.kind == ProfileKind::Shape ?
        static_cast<bool>(pbmodulation::EncodeShapeChromaFrame(bootstrap, data, raster)) :
        profile.kind == ProfileKind::LowFps ?
        static_cast<bool>(pbmodulation::EncodeRemoteVisualLowFpsFrame(bootstrap, data, raster)) :
        static_cast<bool>(pbmodulation::EncodeDesktopLevelsFrame(bootstrap, data, raster));
    if (!encoded)
    {
        throw std::runtime_error("canonical evidence raster generation failed");
    }
    return raster;
}

[[nodiscard]] std::string Hex(const std::span<const std::byte> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string output(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        const unsigned int value = std::to_integer<unsigned int>(bytes[index]);
        output[index * 2] = digits[value >> 4];
        output[index * 2 + 1] = digits[value & 0x0F];
    }
    return output;
}

[[nodiscard]] std::string DescribeStatus(const pbrenderd3d::PresentationStatus& status)
{
    return std::string(pbrenderd3d::GetPresentationErrorName(status.code)) + " stage=" +
        pbrenderd3d::GetPresentationStageName(status.stage) + " native=" + std::to_string(status.nativeError);
}

void Describe(const ProfileBinding& profile, const std::span<const std::byte> raster)
{
    const auto digest = pbprotocol::ComputeBlake3Digest(raster);
    std::cout << "{\"schema\":\"PixelBridge.RemoteVisualEvidenceRaster.1\",\"profile\":\"" << profile.name <<
        "\",\"visualProfileId\":\"0x" << std::hex << profile.visualProfileId << std::dec <<
        "\",\"layoutVersion\":" << static_cast<unsigned int>(profile.layoutVersion) <<
        ",\"width\":" << pbmodulation::kLocalDesktopCanvasWidth <<
        ",\"height\":" << pbmodulation::kLocalDesktopCanvasHeight <<
        ",\"pixelFormat\":\"BGRA8_UNORM\",\"logicalFrameSequence\":17,\"rasterBlake3\":\"" <<
        Hex(digest) << "\"}\n";
}

void Present(const Options& options, const ProfileBinding& profile, const std::span<const std::byte> raster)
{
    pbrenderd3d::DataWindowConfig config;
    config.width = pbmodulation::kLocalDesktopCanvasWidth;
    config.height = pbmodulation::kLocalDesktopCanvasHeight;
    config.clientOrigin = options.origin;
    auto created = pbrenderd3d::DataWindow::Create(config);
    if (!created)
    {
        throw std::runtime_error("DataWindow creation failed: " + DescribeStatus(created.Error()));
    }
    std::unique_ptr<pbrenderd3d::DataWindow> window = std::move(created).Value();
    const auto started = std::chrono::steady_clock::now();
    const auto readyDeadline = started + std::chrono::seconds(15);
    const auto stopAt = started + std::chrono::seconds(options.seconds);
    auto nextSubmit = started;
    std::uint64_t submissions = 0;
    bool readyReported = false;
    while (std::chrono::steady_clock::now() < stopAt)
    {
        const auto now = std::chrono::steady_clock::now();
        const auto snapshot = window->GetSnapshot();
        if (snapshot.state == pbrenderd3d::WindowState::Failed)
        {
            throw std::runtime_error("DataWindow failed: " + DescribeStatus(snapshot.error));
        }
        if (snapshot.state == pbrenderd3d::WindowState::Stopped)
        {
            throw std::runtime_error("DataWindow stopped before the requested evidence dwell completed");
        }
        if (snapshot.candidateContractSatisfied && snapshot.state == pbrenderd3d::WindowState::Running)
        {
            if (snapshot.environment.clientOrigin != options.origin ||
                snapshot.environment.clientWidth != config.width || snapshot.environment.clientHeight != config.height ||
                !snapshot.environment.singleMonitor)
            {
                throw std::runtime_error("DataWindow physical geometry differs from the requested single-monitor ROI");
            }
            if (now >= nextSubmit && !snapshot.pendingFrame)
            {
                const auto status = window->SubmitFrame({raster, config.width, config.height,
                    static_cast<std::size_t>(config.width) * 4, submissions, snapshot.timing.presentationEpoch});
                if (!status && status.code != pbrenderd3d::PresentationErrorCode::EpochMismatch &&
                    status.code != pbrenderd3d::PresentationErrorCode::Paused)
                {
                    throw std::runtime_error("DataWindow submit failed: " + DescribeStatus(status));
                }
                if (status)
                {
                    submissions++;
                    nextSubmit = now + std::chrono::milliseconds(200);
                }
            }
        }
        if (!readyReported && snapshot.totalSuccessfulPresents != 0)
        {
            readyReported = true;
            std::cout << "READY profile=" << profile.name << " origin=" << options.origin.x << ',' << options.origin.y <<
                " size=" << config.width << 'x' << config.height << " dpi=" << snapshot.environment.dpi <<
                " repeatedImmutableRaster=true\n" << std::flush;
        }
        if (!readyReported && now >= readyDeadline)
        {
            throw std::runtime_error("DataWindow did not produce a successful Present within 15 seconds");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    window->Stop();
    const auto finalSnapshot = window->GetSnapshot();
    if (!readyReported || finalSnapshot.totalSuccessfulPresents == 0)
    {
        throw std::runtime_error("evidence presentation ended without a successful Present");
    }
    std::cout << "COMPLETE profile=" << profile.name << " submitted=" << submissions <<
        " successfulPresents=" << finalSnapshot.totalSuccessfulPresents << "\n";
}

void Usage()
{
    std::cerr << "usage: PBRemoteVisualEvidencePresenter describe --profile direct|shape|lf4\n"
                 "       PBRemoteVisualEvidencePresenter present --profile direct|shape|lf4 "
                 "--origin X Y --seconds 1..600\n";
}

} // namespace

int main(const int argumentCount, const char* const arguments[])
{
    Options options;
    if (!ParseOptions(argumentCount, arguments, options))
    {
        Usage();
        return 2;
    }
    try
    {
        const ProfileBinding profile = GetProfile(options.profile);
        const std::vector<std::byte> raster = BuildRaster(profile);
        if (options.mode == Mode::Describe)
        {
            Describe(profile, raster);
        }
        else
        {
            Present(options, profile, raster);
        }
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "PBRemoteVisualEvidencePresenter failed: " << exception.what() << '\n';
        return 1;
    }
}
