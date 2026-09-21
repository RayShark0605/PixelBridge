#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pbencoder
{

struct ShellCommandRegistration
{
    std::wstring registryPath;
    std::wstring command;
};

struct ShellRegistrationStatus
{
    bool success = true;
    std::wstring message;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return success;
    }

    [[nodiscard]] static ShellRegistrationStatus Failure(std::wstring failureMessage)
    {
        return {false, std::move(failureMessage)};
    }
};

[[nodiscard]] std::wstring QuoteWindowsCommandLineArgument(std::wstring_view value);

[[nodiscard]] std::vector<ShellCommandRegistration> BuildShellCommandRegistrations(
    std::wstring_view executablePath);

[[nodiscard]] bool ParseShellOpenArguments(int argumentCount, const wchar_t* const arguments[],
    std::wstring& selectedPath) noexcept;

[[nodiscard]] ShellRegistrationStatus EnsureShellContextMenuRegistration(
    std::wstring_view executablePath) noexcept;

} // namespace pbencoder
