// PBGoldenVectorCheck: golden vector verification harness.
//
// Usage: PBGoldenVectorCheck <golden-root>
//   <golden-root> is the tests/golden directory containing
//   <category>/<name>.bin files.
//
// Verifies every registry vector (file digest vs registry pin, recompute,
// byte comparison with offset/expected/actual on mismatch), every frame
// pin (raw PBRW + PNG digests re-encoded through the current
// implementation). The 275-byte PBVM manifest is part of the ordinary
// file-backed registry rather than a digest-only exception.
//
// Exit codes: 0 = all match, 1 = at least one failure, 2 = usage error.
// stdout is deterministic (registry ordering, no timestamps).

#include "golden_vector_check_core.h"

#include <exception>
#include <filesystem>
#include <iostream>

namespace {

int RunMain(const int argumentCount, char* arguments[])
{
    if (argumentCount != 2)
    {
        std::cerr << "usage: PBGoldenVectorCheck <golden-root>\n";
        return 2;
    }
    const std::filesystem::path goldenRoot = arguments[1];
    std::error_code errorCode;
    if (!std::filesystem::is_directory(goldenRoot, errorCode))
    {
        std::cerr << "[GOLDEN] FAIL golden root is not a directory: "
                  << goldenRoot.generic_string() << "\n";
        return 2;
    }
    const int failureCount =
        pbgoldenchk::RunFullCheck(goldenRoot, std::cout);
    return failureCount == 0 ? 0 : 1;
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
        std::cerr << "[GOLDEN] FAIL kind=UnhandledException detail="
                  << exception.what() << "\n";
    }
    catch (...)
    {
        std::cerr << "[GOLDEN] FAIL kind=UnhandledException detail=unknown\n";
    }
    return 1;
}
