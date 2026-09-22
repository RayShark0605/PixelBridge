#include "encoder_shell_integration.h"
#include "encoder_shell_dialog.h"
#include "folder_archive.h"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include <string>

namespace
{

void WriteFile(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    REQUIRE(file.write(contents) == contents.size());
    REQUIRE(file.flush());
}

} // namespace

TEST_CASE("Encoder shell registration quotes executable and covers files and directories", "[encoder][shell]")
{
    const std::wstring executablePath = L"C:\\Program Files\\PixelBridge\\PixelBridgeEncoder.exe";
    const auto registrations = pbencoder::BuildShellCommandRegistrations(executablePath);

    REQUIRE(registrations.size() == 2);
    REQUIRE(registrations[0].registryPath == L"Software\\Classes\\*\\shell\\PixelBridgeEncoder");
    REQUIRE(registrations[1].registryPath == L"Software\\Classes\\Directory\\shell\\PixelBridgeEncoder");
    REQUIRE(registrations[0].command ==
        L"\"C:\\Program Files\\PixelBridge\\PixelBridgeEncoder.exe\" --shell-open \"%1\"");
    REQUIRE(registrations[1].command == registrations[0].command);
}

TEST_CASE("Encoder shell invocation accepts exactly one existing path argument", "[encoder][shell]")
{
    const wchar_t option[] = L"--shell-open";
    const wchar_t path[] = L"C:\\data\\folder";
    const wchar_t* validArguments[] = {L"PixelBridgeEncoder.exe", option, path};
    std::wstring parsedPath;
    REQUIRE(pbencoder::ParseShellOpenArguments(3, validArguments, parsedPath));
    REQUIRE(parsedPath == path);

    const wchar_t* missingPath[] = {L"PixelBridgeEncoder.exe", option};
    REQUIRE_FALSE(pbencoder::ParseShellOpenArguments(2, missingPath, parsedPath));

    const wchar_t extra[] = L"extra";
    const wchar_t* extraArguments[] = {L"PixelBridgeEncoder.exe", option, path, extra};
    REQUIRE_FALSE(pbencoder::ParseShellOpenArguments(4, extraArguments, parsedPath));
}

TEST_CASE("Active shell dialog cancellation requests a safe stop instead of rejecting", "[encoder][shell][lifecycle]")
{
    REQUIRE(pbencoder::GetShellDialogCancelAction(true) == pbencoder::ShellDialogCancelAction::RequestStop);
    REQUIRE(pbencoder::GetShellDialogCancelAction(false) == pbencoder::ShellDialogCancelAction::Reject);
}

TEST_CASE("Shell registration does not label an unowned conflicting command", "[encoder][shell][registry]")
{
    REQUIRE_FALSE(pbencoder::ShouldPublishShellRegistrationDisplayName(false, false));
    REQUIRE(pbencoder::ShouldPublishShellRegistrationDisplayName(true, false));
    REQUIRE_FALSE(pbencoder::ShouldPublishShellRegistrationDisplayName(true, true));
}

TEST_CASE("Folder archive creates a standard compressed archive without changing the source", "[encoder][folder]")
{
    QTemporaryDir scratch;
    REQUIRE(scratch.isValid());
    QDir root(scratch.path());
    REQUIRE(root.mkpath(QStringLiteral("source/sub")));
    const QString sourcePath = root.filePath(QStringLiteral("source"));
    WriteFile(root.filePath(QStringLiteral("source/alpha.txt")), QByteArray("alpha"));
    WriteFile(root.filePath(QStringLiteral("source/sub/beta.txt")), QByteArray("beta"));
    const QString hiddenPath = root.filePath(QStringLiteral("source/.hidden.txt"));
    WriteFile(hiddenPath, QByteArray("hidden"));
    REQUIRE(SetFileAttributesW(reinterpret_cast<const wchar_t*>(hiddenPath.utf16()), FILE_ATTRIBUTE_HIDDEN) != FALSE);

    const auto result = pbencoder::CreateFolderArchive(sourcePath);
    INFO(result.errorMessage.toStdString());
    REQUIRE(result.success);
    REQUIRE(!result.archivePath.isEmpty());
    const QFileInfo archiveInfo(result.archivePath);
    REQUIRE(archiveInfo.isFile());
    REQUIRE(archiveInfo.size() > 0);
    REQUIRE(QFileInfo::exists(root.filePath(QStringLiteral("source/alpha.txt"))));
    REQUIRE(QFileInfo::exists(root.filePath(QStringLiteral("source/sub/beta.txt"))));

    QProcess listing;
    listing.start(QStringLiteral("tar"), {QStringLiteral("-tf"), result.archivePath});
    REQUIRE(listing.waitForFinished(30000));
    REQUIRE(listing.exitStatus() == QProcess::NormalExit);
    REQUIRE(listing.exitCode() == 0);
    const QString listingText = QString::fromLocal8Bit(listing.readAllStandardOutput());
    REQUIRE(listingText.contains(QStringLiteral("source/alpha.txt")));
    REQUIRE(listingText.contains(QStringLiteral("source/sub/beta.txt")));
    REQUIRE(listingText.contains(QStringLiteral("source/.hidden.txt")));

    static_cast<void>(SetFileAttributesW(reinterpret_cast<const wchar_t*>(hiddenPath.utf16()), FILE_ATTRIBUTE_NORMAL));
    REQUIRE(QFile::remove(result.archivePath));
}

TEST_CASE("Folder archive rejects a regular file input", "[encoder][folder][negative]")
{
    QTemporaryDir scratch;
    REQUIRE(scratch.isValid());
    const QString filePath = QDir(scratch.path()).filePath(QStringLiteral("source.txt"));
    WriteFile(filePath, QByteArray("not-a-folder"));

    const auto result = pbencoder::CreateFolderArchive(filePath);
    REQUIRE_FALSE(result.success);
    REQUIRE(result.archivePath.isEmpty());
    REQUIRE_FALSE(result.errorMessage.isEmpty());
}

TEST_CASE("Folder archive treats a leading-dash directory name as data", "[encoder][folder][arguments]")
{
    QTemporaryDir scratch;
    REQUIRE(scratch.isValid());
    QDir root(scratch.path());
    const QString folderName = QStringLiteral("-source");
    REQUIRE(root.mkpath(folderName));
    const QString sourcePath = root.filePath(folderName);
    WriteFile(root.filePath(folderName + QStringLiteral("/payload.txt")), QByteArray("payload"));

    const auto result = pbencoder::CreateFolderArchive(sourcePath);
    INFO(result.errorMessage.toStdString());
    REQUIRE(result.success);

    QProcess listing;
    listing.start(QStringLiteral("tar"), {QStringLiteral("-tf"), result.archivePath});
    REQUIRE(listing.waitForFinished(30000));
    REQUIRE(listing.exitStatus() == QProcess::NormalExit);
    REQUIRE(listing.exitCode() == 0);
    const QString listingText = QString::fromLocal8Bit(listing.readAllStandardOutput());
    REQUIRE(listingText.contains(QStringLiteral("-source/payload.txt")));
    REQUIRE(QFile::remove(result.archivePath));
}
