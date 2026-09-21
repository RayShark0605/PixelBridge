#include "encoder_shell_integration.h"

#include <Windows.h>
#include <Shellapi.h>
#include <ShlObj.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace pbencoder
{

namespace
{

constexpr wchar_t registrationOwnerValueName[] = L"PBRegistrationOwner";
constexpr wchar_t registrationOwnerValue[] = L"PixelBridgeEncoder.1";

class RegistryKey final
{
public:
    RegistryKey() noexcept = default;
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    ~RegistryKey()
    {
        Reset();
    }

    [[nodiscard]] HKEY Get() const noexcept
    {
        return key_;
    }

    void Reset(HKEY key = nullptr) noexcept
    {
        if (key_ != nullptr)
        {
            RegCloseKey(key_);
        }
        key_ = key;
    }

private:
    HKEY key_ = nullptr;
};

[[nodiscard]] std::wstring NativeRegistryError(const wchar_t* operation, const LSTATUS error)
{
    return std::wstring(operation) + L" 失败（Win32=" + std::to_wstring(static_cast<unsigned long>(error)) + L"）";
}

[[nodiscard]] ShellRegistrationStatus SetStringValue(HKEY key, const wchar_t* valueName,
    const std::wstring& value)
{
    if (value.size() > (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t) - 1U)
    {
        return ShellRegistrationStatus::Failure(L"右键菜单命令过长");
    }
    const std::size_t byteCount = (value.size() + 1U) * sizeof(wchar_t);
    if (byteCount > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()))
    {
        return ShellRegistrationStatus::Failure(L"右键菜单命令过长");
    }
    const LSTATUS status = RegSetValueExW(key, valueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>(byteCount));
    return status == ERROR_SUCCESS ? ShellRegistrationStatus{} :
        ShellRegistrationStatus::Failure(NativeRegistryError(L"写入右键菜单注册表", status));
}

[[nodiscard]] bool HasNonEmptyDefaultValue(HKEY key)
{
    DWORD valueType = 0;
    DWORD byteCount = 0;
    const LSTATUS queryStatus = RegQueryValueExW(key, nullptr, nullptr, &valueType, nullptr, &byteCount);
    if (queryStatus != ERROR_SUCCESS || (valueType != REG_SZ && valueType != REG_EXPAND_SZ) || byteCount < sizeof(wchar_t))
    {
        return false;
    }
    if (byteCount > 64U * 1024U)
    {
        return false;
    }
    std::wstring value(byteCount / sizeof(wchar_t), L'\0');
    DWORD queriedBytes = byteCount;
    if (RegQueryValueExW(key, nullptr, nullptr, &valueType, reinterpret_cast<BYTE*>(value.data()), &queriedBytes) != ERROR_SUCCESS)
    {
        return false;
    }
    if (!value.empty() && value.back() == L'\0')
    {
        value.pop_back();
    }
    return !value.empty();
}

[[nodiscard]] bool HasRegistrationOwner(HKEY key)
{
    DWORD valueType = 0;
    DWORD byteCount = 0;
    if (RegQueryValueExW(key, registrationOwnerValueName, nullptr, &valueType, nullptr, &byteCount) != ERROR_SUCCESS ||
        (valueType != REG_SZ && valueType != REG_EXPAND_SZ) || byteCount < sizeof(wchar_t))
    {
        return false;
    }
    if (byteCount > 64U * 1024U)
    {
        return false;
    }
    std::wstring value(byteCount / sizeof(wchar_t), L'\0');
    DWORD queriedBytes = byteCount;
    if (RegQueryValueExW(key, registrationOwnerValueName, nullptr, &valueType,
        reinterpret_cast<BYTE*>(value.data()), &queriedBytes) != ERROR_SUCCESS)
    {
        return false;
    }
    if (!value.empty() && value.back() == L'\0')
    {
        value.pop_back();
    }
    return value == registrationOwnerValue;
}

[[nodiscard]] ShellRegistrationStatus EnsureSingleRegistration(const ShellCommandRegistration& registration,
    bool& changed)
{
    changed = false;
    RegistryKey shellKey;
    HKEY rawShellKey = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, registration.registryPath.c_str(), 0,
        KEY_READ | KEY_WRITE, &rawShellKey);
    shellKey.Reset(rawShellKey);
    if (status == ERROR_FILE_NOT_FOUND)
    {
        HKEY createdShellKey = nullptr;
        DWORD disposition = 0;
        status = RegCreateKeyExW(HKEY_CURRENT_USER, registration.registryPath.c_str(), 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, nullptr, &createdShellKey, &disposition);
        if (status != ERROR_SUCCESS)
        {
            return ShellRegistrationStatus::Failure(NativeRegistryError(L"创建右键菜单注册表", status));
        }
        shellKey.Reset(createdShellKey);
        changed = disposition == REG_CREATED_NEW_KEY;
    }
    else if (status != ERROR_SUCCESS)
    {
        return ShellRegistrationStatus::Failure(NativeRegistryError(L"读取右键菜单注册表", status));
    }

    if (!HasNonEmptyDefaultValue(shellKey.Get()))
    {
        const ShellRegistrationStatus displayStatus = SetStringValue(shellKey.Get(), nullptr, L"PixelBridgeEncoder");
        if (!displayStatus)
        {
            return displayStatus;
        }
        changed = true;
    }

    const bool ownedRegistration = HasRegistrationOwner(shellKey.Get());

    RegistryKey commandKey;
    HKEY rawCommandKey = nullptr;
    status = RegOpenKeyExW(shellKey.Get(), L"command", 0, KEY_READ | KEY_WRITE, &rawCommandKey);
    if (status == ERROR_FILE_NOT_FOUND)
    {
        DWORD disposition = 0;
        status = RegCreateKeyExW(shellKey.Get(), L"command", 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_READ | KEY_WRITE, nullptr, &rawCommandKey, &disposition);
        if (status != ERROR_SUCCESS)
        {
            return ShellRegistrationStatus::Failure(NativeRegistryError(L"创建右键菜单命令", status));
        }
        commandKey.Reset(rawCommandKey);
        const ShellRegistrationStatus commandStatus = SetStringValue(commandKey.Get(), nullptr, registration.command);
        if (!commandStatus)
        {
            return commandStatus;
        }
        const ShellRegistrationStatus modelStatus = SetStringValue(commandKey.Get(), L"MultiSelectModel", L"Single");
        if (!modelStatus)
        {
            return modelStatus;
        }
        const ShellRegistrationStatus ownerStatus = SetStringValue(shellKey.Get(), registrationOwnerValueName, registrationOwnerValue);
        if (!ownerStatus)
        {
            return ownerStatus;
        }
        changed = true;
    }
    else if (status != ERROR_SUCCESS)
    {
        return ShellRegistrationStatus::Failure(NativeRegistryError(L"读取右键菜单命令", status));
    }
    else
    {
        commandKey.Reset(rawCommandKey);
        // A non-empty existing command belongs to the user's current shell
        // registration. Never overwrite it silently; this keeps the startup
        // repair idempotent and avoids hijacking a deliberately customized verb.
        if (!HasNonEmptyDefaultValue(commandKey.Get()) || ownedRegistration)
        {
            const ShellRegistrationStatus commandStatus = SetStringValue(commandKey.Get(), nullptr, registration.command);
            if (!commandStatus)
            {
                return commandStatus;
            }
            const ShellRegistrationStatus modelStatus = SetStringValue(commandKey.Get(), L"MultiSelectModel", L"Single");
            if (!modelStatus)
            {
                return modelStatus;
            }
            const ShellRegistrationStatus ownerStatus = SetStringValue(shellKey.Get(), registrationOwnerValueName, registrationOwnerValue);
            if (!ownerStatus)
            {
                return ownerStatus;
            }
            changed = true;
        }
    }
    return {};
}

} // namespace

std::wstring QuoteWindowsCommandLineArgument(const std::wstring_view value)
{
    std::wstring quoted;
    quoted.reserve(value.size() + 2U);
    quoted.push_back(L'"');
    std::size_t backslashCount = 0;
    for (const wchar_t character : value)
    {
        if (character == L'\\')
        {
            backslashCount++;
            continue;
        }
        if (character == L'"')
        {
            quoted.append(backslashCount * 2U + 1U, L'\\');
            quoted.push_back(L'"');
            backslashCount = 0;
            continue;
        }
        quoted.append(backslashCount, L'\\');
        quoted.push_back(character);
        backslashCount = 0;
    }
    quoted.append(backslashCount * 2U, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::vector<ShellCommandRegistration> BuildShellCommandRegistrations(const std::wstring_view executablePath)
{
    const std::wstring command = QuoteWindowsCommandLineArgument(executablePath) + L" --shell-open \"%1\"";
    return {
        {L"Software\\Classes\\*\\shell\\PixelBridgeEncoder", command},
        {L"Software\\Classes\\Directory\\shell\\PixelBridgeEncoder", command}
    };
}

bool ParseShellOpenArguments(const int argumentCount, const wchar_t* const arguments[], std::wstring& selectedPath) noexcept
{
    selectedPath.clear();
    if (argumentCount != 3 || arguments == nullptr || arguments[1] == nullptr || arguments[2] == nullptr ||
        std::wstring_view(arguments[1]) != L"--shell-open" || arguments[2][0] == L'\0')
    {
        return false;
    }
    try
    {
        selectedPath = arguments[2];
        return selectedPath.size() <= 32768U;
    }
    catch (...)
    {
        selectedPath.clear();
        return false;
    }
}

ShellRegistrationStatus EnsureShellContextMenuRegistration(const std::wstring_view executablePath) noexcept
{
    if (executablePath.empty())
    {
        return ShellRegistrationStatus::Failure(L"无法确定 PixelBridgeEncoder.exe 路径");
    }
    try
    {
        bool changed = false;
        for (const ShellCommandRegistration& registration : BuildShellCommandRegistrations(executablePath))
        {
            bool registrationChanged = false;
            const ShellRegistrationStatus status = EnsureSingleRegistration(registration, registrationChanged);
            if (!status)
            {
                return status;
            }
            changed = changed || registrationChanged;
        }
        if (changed)
        {
            SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        }
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return ShellRegistrationStatus::Failure(L"注册右键菜单时内存不足");
    }
    catch (...)
    {
        return ShellRegistrationStatus::Failure(L"注册右键菜单时发生未知错误");
    }
}

} // namespace pbencoder
