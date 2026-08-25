// PBProtocolDump: deterministic protocol record inspector (CLI).
//
// Usage:
//   PBProtocolDump [options] <file>
//
// Options:
//   --type <name>              Record type to force; default "auto"
//                              (magic dispatch + Transport probe). Values:
//                              auto, bootstrap, control, fragment,
//                              session-descriptor, segment-descriptor,
//                              final-manifest, transport,
//                              wirehair-descriptor, pbvm-manifest.
//   --session-descriptor <f>   37-byte session-descriptor payload used as
//                              the context for the control type-2/type-3
//                              and standalone segment-descriptor /
//                              final-manifest semantic cross-checks.
//
// Input domain: a single record file of at most 65,571 bytes (the maximum
// supported record size); larger files are rejected as usage errors
// before any parsing (no unbounded input loading).
//
// Output: a deterministic field table (see protocol_dump_core.h); no
// timestamps, byte-stable across runs.
//
// Exit codes:
//   0  the record parsed successfully (authoritative parse passed)
//   1  the record was evaluated but failed to parse (including
//      unrecognized input and session-tag / manifest mismatches)
//   2  usage or I/O error (bad arguments, missing/unreadable file,
//      oversized input, invalid session-descriptor size)

#include "protocol_dump_core.h"

#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/transport_block_codec.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kMaximumRecordBytes =
    pbprotocol::kTransportMaximumBlockBytes; // 65,571
constexpr std::size_t kSessionDescriptorBytes =
    pbprotocol::kSessionDescriptorPayloadBytes; // 37

[[nodiscard]] bool IsValidTypeHint(const std::string& typeHint)
{
    static constexpr const char* const kKnownTypes[] = {
        "auto",
        "bootstrap",
        "control",
        "fragment",
        "session-descriptor",
        "segment-descriptor",
        "final-manifest",
        "transport",
        "wirehair-descriptor",
        "pbvm-manifest"};
    for (const char* knownType : kKnownTypes)
    {
        if (typeHint == knownType)
        {
            return true;
        }
    }
    return false;
}

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
    if (size < 0)
    {
        return false;
    }
    if (static_cast<std::size_t>(size) > kMaximumRecordBytes)
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

void PrintUsage(std::ostream& out)
{
    out << "usage: PBProtocolDump [--type <name>] "
           "[--session-descriptor <file>] <file>\n";
}

} // namespace

int RunMain(const int argumentCount, char* arguments[])
{
    std::string typeHint = "auto";
    std::string sessionDescriptorPath;
    std::string recordPath;
    bool usageError = false;
    bool typeWasSet = false;
    bool contextWasSet = false;
    for (int i = 1; i < argumentCount; i++)
    {
        const std::string argument = arguments[i];
        if (argument == "--type" && i + 1 < argumentCount && !typeWasSet)
        {
            i++;
            typeHint = arguments[i];
            typeWasSet = true;
        }
        else if (argument == "--session-descriptor" &&
            i + 1 < argumentCount && !contextWasSet)
        {
            i++;
            sessionDescriptorPath = arguments[i];
            contextWasSet = true;
        }
        else if (!argument.empty() && argument[0] == '-')
        {
            usageError = true;
            break;
        }
        else if (recordPath.empty())
        {
            recordPath = argument;
        }
        else
        {
            usageError = true;
            break;
        }
    }
    if (usageError || recordPath.empty() || !IsValidTypeHint(typeHint))
    {
        PrintUsage(std::cerr);
        return 2;
    }

    std::vector<std::byte> recordBytes;
    if (!ReadFileBytes(recordPath, recordBytes))
    {
        std::cerr << "[error] cannot read record file (missing, unreadable, "
                     "or larger than " << kMaximumRecordBytes
                  << " bytes): " << recordPath << "\n";
        return 2;
    }

    std::vector<std::byte> sessionBytes;
    if (!sessionDescriptorPath.empty())
    {
        if (!ReadFileBytes(sessionDescriptorPath, sessionBytes) ||
            sessionBytes.size() != kSessionDescriptorBytes)
        {
            std::cerr << "[error] session-descriptor file must hold exactly "
                         << kSessionDescriptorBytes
                      << " bytes: " << sessionDescriptorPath << "\n";
            return 2;
        }
    }

    const pbdump::DumpReport report = pbdump::DumpRecord(
        std::span<const std::byte>(recordBytes), typeHint,
        std::span<const std::byte>(sessionBytes));
    std::cout << pbdump::FormatDump(report);
    return report.parseOk ? 0 : 1;
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
