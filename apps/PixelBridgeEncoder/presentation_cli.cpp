#include "pbrenderd3d/data_window.h"
#include "pbmodulation/reference_raster.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/session_random.h"

#include <Windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

void Usage()
{
    std::cout << "Usage: PixelBridgeEncoder --data-window [--frames N] [--telemetry NEW_FILE.jsonl]\n"
              << "N bounds accepted CPU frame submissions (1..1000000), not displayed frames.\n"
              << "Without --frames, press Escape in the data window to stop.\n"
              << "Reference-raster presentation only; not a file sender or capture certification.\n";
}

bool ParseFrameCount(const std::wstring_view text, std::uint64_t& result)
{
    if (text.empty())
    {
        return false;
    }
    std::uint64_t value = 0;
    for (const wchar_t character : text)
    {
        if (character < L'0' || character > L'9')
        {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - L'0');
        if (value > (1000000 - digit) / 10)
        {
            return false;
        }
        value = value * 10 + digit;
    }
    if (value == 0)
    {
        return false;
    }
    result = value;
    return true;
}

class DiagnosticFile
{
public:
    explicit DiagnosticFile(const wchar_t* path)
    {
        if (path == nullptr)
        {
            return;
        }
        handle_ = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("cannot create new telemetry file; win32=" + std::to_string(GetLastError()));
        }
    }
    DiagnosticFile(const DiagnosticFile&) = delete;
    DiagnosticFile& operator=(const DiagnosticFile&) = delete;
    ~DiagnosticFile()
    {
        if (handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle_);
        }
    }
    void Write(const pbrenderd3d::DataWindowSnapshot& snapshot)
    {
        if (handle_ == INVALID_HANDLE_VALUE)
        {
            return;
        }
        std::ostringstream stream;
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        pbrenderd3d::WriteDataWindowSnapshotJson(stream, snapshot);
        stream << '\n';
        const std::string text = stream.str();
        constexpr std::size_t maximumLogBytes = 16 * 1024 * 1024;
        if (text.size() > maximumLogBytes - bytesWritten_)
        {
            throw std::runtime_error("telemetry reached its 16 MiB limit; stopping instead of growing an unbounded log");
        }
        DWORD written = 0;
        if (!WriteFile(handle_, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) || written != text.size())
        {
            throw std::runtime_error("telemetry write failed");
        }
        bytesWritten_ += written;
    }
    void Finish()
    {
        if (handle_ != INVALID_HANDLE_VALUE)
        {
            if (!FlushFileBuffers(handle_))
            {
                throw std::runtime_error("telemetry flush failed");
            }
            const HANDLE handle = handle_;
            handle_ = INVALID_HANDLE_VALUE;
            if (!CloseHandle(handle))
            {
                throw std::runtime_error("telemetry close failed");
            }
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::size_t bytesWritten_ = 0;
};

}

int RunDataWindowCommand(const int argumentCount, wchar_t* arguments[])
{
    bool dataWindow = false;
    bool hasFrameCount = false;
    std::uint64_t frameLimit = 0;
    const wchar_t* telemetryPath = nullptr;
    if (argumentCount == 2 && std::wstring_view(arguments[1]) == L"--help")
    {
        Usage();
        return 0;
    }
    for (int index = 1; index < argumentCount; index++)
    {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--data-window" && !dataWindow)
        {
            dataWindow = true;
        }
        else if (argument == L"--frames" && !hasFrameCount && index + 1 < argumentCount)
        {
            index++;
            if (!ParseFrameCount(arguments[index], frameLimit))
            {
                Usage();
                return 2;
            }
            hasFrameCount = true;
        }
        else if (argument == L"--telemetry" && telemetryPath == nullptr && index + 1 < argumentCount)
        {
            index++;
            telemetryPath = arguments[index];
            if (*telemetryPath == 0)
            {
                Usage();
                return 2;
            }
        }
        else
        {
            Usage();
            return 2;
        }
    }
    if (!dataWindow)
    {
        Usage();
        return 2;
    }
    try
    {
        DiagnosticFile telemetry(telemetryPath);
        const auto session = pbprotocol::GenerateRandomSessionId();
        if (!session)
        {
            throw std::runtime_error("OS CSPRNG session creation failed");
        }
        const auto sessionTag = pbprotocol::DeriveSessionTag(session.Value());
        auto created = pbrenderd3d::DataWindow::Create({});
        if (!created)
        {
            const auto error = created.Error();
            throw std::runtime_error(std::string(pbrenderd3d::GetPresentationErrorName(error.code)) + ":" + pbrenderd3d::GetPresentationStageName(error.stage) +
                                     ":" + std::to_string(error.nativeError));
        }
        const auto window = std::move(created).Value();
        std::array<std::byte, pbmodulation::kReferenceBootstrapRecordBytes> bootstrapBytes{};
        std::array<std::byte, pbmodulation::kReferenceControlWindowBytes> control{};
        std::vector<std::byte> data(pbmodulation::kReferenceDataRegionBytes);
        std::vector<std::byte> pixels(pbmodulation::kReferenceFrameBgraBytes);
        for (std::size_t index = 0; index < data.size(); index++)
        {
            data[index] = static_cast<std::byte>((index * 37 + 5) & 255);
        }
        std::uint64_t sequence = 0;
        std::uint64_t lastPresents = 0;
        auto lastProgress = std::chrono::steady_clock::now();
        auto lastLog = lastProgress;
        for (;;)
        {
            const auto snapshot = window->GetSnapshot();
            const auto now = std::chrono::steady_clock::now();
            if (snapshot.state == pbrenderd3d::WindowState::Failed)
            {
                telemetry.Write(snapshot);
                throw std::runtime_error(std::string(pbrenderd3d::GetPresentationErrorName(snapshot.error.code)) + ":" +
                                         pbrenderd3d::GetPresentationStageName(snapshot.error.stage) + ":" + std::to_string(snapshot.error.nativeError));
            }
            if (snapshot.state == pbrenderd3d::WindowState::Stopped)
            {
                if (hasFrameCount && sequence < frameLimit)
                {
                    throw std::runtime_error("data window closed before the submission budget completed");
                }
                break;
            }
            if (snapshot.totalPresentCalls != lastPresents)
            {
                lastPresents = snapshot.totalPresentCalls;
                lastProgress = now;
            }
            if (now - lastLog >= std::chrono::seconds(1))
            {
                telemetry.Write(snapshot);
                lastLog = now;
            }
            if (hasFrameCount && sequence >= frameLimit && !snapshot.pendingFrame && !snapshot.inFlightFrame)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                break;
            }
            if (hasFrameCount && now - lastProgress > std::chrono::seconds(10))
            {
                throw std::runtime_error("finite presentation run made no progress for 10 seconds");
            }
            if ((!hasFrameCount || sequence < frameLimit) && snapshot.state == pbrenderd3d::WindowState::Running && !snapshot.pendingFrame)
            {
                // This is the existing reference binding, not a new visual
                // profile. Only Bootstrap sequence changes between frames.
                const pbprotocol::BootstrapRecord bootstrap{
                    pbprotocol::kBootstrapVersion, pbprotocol::GetProtocolVersion(), 1, 0x5042524546524153ULL, sessionTag, sequence, 0, 0};
                if (!pbprotocol::SerializeBootstrapRecord(bootstrap, bootstrapBytes) ||
                    !pbmodulation::EncodeReferenceFrame({bootstrapBytes, control, data}, pixels))
                {
                    throw std::runtime_error("canonical reference frame generation failed");
                }
                const auto status =
                    window->SubmitFrame({pixels, pbmodulation::kReferenceCanvasWidth, pbmodulation::kReferenceCanvasHeight,
                                         static_cast<std::size_t>(pbmodulation::kReferenceCanvasWidth) * 4, sequence, snapshot.timing.presentationEpoch});
                if (status)
                {
                    if (sequence == std::numeric_limits<std::uint64_t>::max())
                    {
                        throw std::runtime_error("FrameSequence exhausted; a new Session is required");
                    }
                    sequence++;
                }
                else if (status.code != pbrenderd3d::PresentationErrorCode::EpochMismatch && status.code != pbrenderd3d::PresentationErrorCode::Paused &&
                         status.code != pbrenderd3d::PresentationErrorCode::NotRunning)
                {
                    throw std::runtime_error("reference frame submission failed");
                }
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        const auto finalSample = window->GetSnapshot();
        telemetry.Write(finalSample);
        window->Stop();
        const auto stoppedSample = window->GetSnapshot();
        const bool stopFailed = stoppedSample.state == pbrenderd3d::WindowState::Failed;
        if (stopFailed)
        {
            telemetry.Write(stoppedSample);
        }
        telemetry.Finish();
        pbrenderd3d::WriteDataWindowSnapshotJson(std::cout, stopFailed ? stoppedSample : finalSample);
        std::cout << '\n';
        return stopFailed || finalSample.state == pbrenderd3d::WindowState::Failed ? 1 : 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "PixelBridgeEncoder presentation error: " << exception.what() << '\n';
        return 1;
    }
}
