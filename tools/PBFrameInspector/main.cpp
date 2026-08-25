// PBFrameInspector: deterministic reference-frame inspector (CLI).
//
// Usage:
//   PBFrameInspector [--recovery] <file>
//
// <file> is one frame container: a PBRW raw frame (28-byte header +
// 8,294,400 BGRA bytes) or a PNG of the frozen 1920x1080 reference
// canvas. --recovery runs the fail-closed 2,025-byte Robust codeword
// scan over the decoded data region (syndrome -> bounded perfect-LLR
// decode -> info-block extraction -> strict Transport parse ->
// SessionTag cross-check against the bootstrap record).
//
// Output: deterministic container/canvas/frame/region/bootstrap/
// control/data/recovery report (see frame_inspector_core.h); no
// timestamps, byte-stable across runs.
//
// Input domain: files of at most 16 MiB are loaded; anything larger is
// rejected as a usage error (a reference frame container never exceeds
// a few MiB, so this bounds the tool's memory without weakening any
// protocol check).
//
// Exit codes:
//   0  container + frame (+ recovery when requested) all passed
//   1  the input was evaluated but failed (unrecognized container,
//      geometry mismatch, demod failure, recovery failure, tag
//      mismatch)
//   2  usage or I/O error

#include "frame_inspector_core.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

// 16 MiB tool input cap (see file header).
constexpr std::size_t kMaximumFileBytes = 16u * 1024u * 1024u;

[[nodiscard]] bool ReadFileBytes(
    const std::string& path, std::vector<std::byte>& outBytes)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return false;
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0 ||
        static_cast<std::size_t>(size) > kMaximumFileBytes)
    {
        return false;
    }
    stream.seekg(0, std::ios::beg);
    outBytes.resize(static_cast<std::size_t>(size));
    if (outBytes.empty())
    {
        return true;
    }
    return static_cast<bool>(
        stream.read(reinterpret_cast<char*>(outBytes.data()),
            static_cast<std::streamsize>(outBytes.size())));
}

} // namespace

int RunMain(const int argumentCount, char* arguments[])
{
    bool recoveryRequested = false;
    std::string framePath;
    bool usageError = false;
    for (int i = 1; i < argumentCount; i++)
    {
        const std::string argument = arguments[i];
        if (argument == "--recovery")
        {
            if (recoveryRequested)
            {
                usageError = true;
                break;
            }
            recoveryRequested = true;
        }
        else if (!argument.empty() && argument[0] == '-')
        {
            usageError = true;
            break;
        }
        else if (framePath.empty())
        {
            framePath = argument;
        }
        else
        {
            usageError = true;
            break;
        }
    }
    if (usageError || framePath.empty())
    {
        std::cerr << "usage: PBFrameInspector [--recovery] <file>\n";
        return 2;
    }

    std::vector<std::byte> frameBytes;
    if (!ReadFileBytes(framePath, frameBytes))
    {
        std::cerr << "[error] cannot read frame file (missing, unreadable, "
                     "or larger than " << kMaximumFileBytes
                  << " bytes): " << framePath << "\n";
        return 2;
    }

    pbinspect::InspectorReport report;
    pbinspect::InspectFrame(
        std::span<const std::byte>(frameBytes), recoveryRequested,
        report);
    std::cout << pbinspect::FormatReport(report);
    const bool overallOk =
        report.containerOk && report.frameOk && report.bootstrapParsed &&
        (!recoveryRequested ||
            (report.recoveryOk && report.sessionTagCrossCheckOk));
    return overallOk ? 0 : 1;
}

int main(const int argumentCount, char* arguments[])
{
    try
    {
        return RunMain(argumentCount, arguments);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[error] exception: " << exception.what() << "\n";
    }
    catch (...)
    {
        std::cerr << "[error] unknown exception\n";
    }
    return 2;
}
