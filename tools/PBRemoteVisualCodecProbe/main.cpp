#include "codec_probe_core.h"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace
{

[[nodiscard]] bool ReadEvaluationInput(const std::filesystem::path& path,
    std::vector<std::byte>& output) noexcept
{
    try
    {
        std::error_code filesystemError;
        const std::uintmax_t fileBytes = std::filesystem::file_size(path, filesystemError);
        const std::uint64_t maximumBytes = static_cast<std::uint64_t>(
            pbremotevisualcodecprobe::kGrayFrameBytes) * pbremotevisualcodecprobe::kMaximumEvaluationFrames;
        if (filesystemError || fileBytes == 0 || fileBytes > maximumBytes ||
            fileBytes > static_cast<std::uintmax_t>((std::numeric_limits<std::size_t>::max)()))
        {
            return false;
        }
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            return false;
        }
        output.resize(static_cast<std::size_t>(fileBytes));
        stream.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
        return static_cast<bool>(stream) && stream.peek() == std::char_traits<char>::eof();
    }
    catch (...)
    {
        return false;
    }
}

int RunMain(const int argumentCount, char* arguments[])
{
    if (argumentCount == 2 && std::string(arguments[1]) == "export-bgra-sequence")
    {
        pbremotevisualcodecprobe::CodecSourceSequence source;
        std::string error;
        if (!pbremotevisualcodecprobe::BuildCanonicalSourceSequence(source, error))
        {
            std::cerr << "[error] " << error << "\n";
            return 1;
        }
#if defined(_WIN32)
        if (_setmode(_fileno(stdout), _O_BINARY) == -1)
        {
            std::cerr << "[error] cannot set stdout binary mode\n";
            return 1;
        }
#endif
        std::cout.write(reinterpret_cast<const char*>(source.bgraFrames.data()),
            static_cast<std::streamsize>(source.bgraFrames.size()));
        std::cout.flush();
        return std::cout ? 0 : 1;
    }
    if (argumentCount == 2 && std::string(arguments[1]) == "describe-source")
    {
        pbremotevisualcodecprobe::CodecSourceSequence source;
        std::string error;
        if (!pbremotevisualcodecprobe::BuildCanonicalSourceSequence(source, error))
        {
            std::cerr << "[error] " << error << "\n";
            return 1;
        }
        std::cout << source.canonicalManifestJson;
        return std::cout ? 0 : 1;
    }
    if (argumentCount == 3 && std::string(arguments[1]) == "evaluate-gray8-sequence")
    {
        std::vector<std::byte> frames;
        if (!ReadEvaluationInput(arguments[2], frames))
        {
            std::cerr << "[error] cannot read a bounded Gray8 frame sequence: " << arguments[2] << "\n";
            return 2;
        }
        pbremotevisualcodecprobe::CodecSequenceEvaluation evaluation;
        std::string error;
        if (!pbremotevisualcodecprobe::EvaluateGray8Sequence(frames, evaluation, error))
        {
            std::cerr << "[error] " << error << "\n";
            return 1;
        }
        std::cout << evaluation.canonicalJson;
        return evaluation.truthBoundaryValid ? 0 : 1;
    }
    std::cerr << "usage: PBRemoteVisualCodecProbe <describe-source|export-bgra-sequence|"
        "evaluate-gray8-sequence <gray8-file>>\n";
    return 2;
}

} // namespace

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
    return 1;
}
