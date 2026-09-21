#include "encoder_shell_integration.h"
#include "folder_archive.h"

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("Folder archive creates a standard compressed archive without changing the source", "[encoder][folder]")
{
    QTemporaryDir scratch;
    REQUIRE(scratch.isValid());
    QDir root(scratch.path());
    REQUIRE(root.mkpath(QStringLiteral("source/sub")));
    const QString sourcePath = root.filePath(QStringLiteral("source"));
    WriteFile(root.filePath(QStringLiteral("source/alpha.txt")), QByteArray("alpha"));
    WriteFile(root.filePath(QStringLiteral("source/sub/beta.txt")), QByteArray("beta"));

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
