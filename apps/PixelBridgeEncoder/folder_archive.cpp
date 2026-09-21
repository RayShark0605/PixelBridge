#include "folder_archive.h"

#include <Windows.h>

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUuid>

#include <cstdint>
#include <limits>

namespace pbencoder
{

namespace
{

constexpr std::uint64_t maximumArchiveEntries = 1000000;
constexpr std::uint64_t maximumArchiveBytes = 500ULL * 1024ULL * 1024ULL * 1024ULL;

[[nodiscard]] bool IsReparsePoint(const QString& path)
{
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<const wchar_t*>(path.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

[[nodiscard]] FolderArchiveResult Failure(const QString& message)
{
    return {false, {}, message};
}

[[nodiscard]] bool AddChecked(const std::uint64_t left, const std::uint64_t right, std::uint64_t& output) noexcept
{
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left)
    {
        return false;
    }
    output = left + right;
    return true;
}

[[nodiscard]] FolderArchiveResult ValidateFolderTree(const QString& folderPath)
{
    const QFileInfo rootInfo(folderPath);
    if (!rootInfo.exists() || !rootInfo.isDir() || !rootInfo.isReadable())
    {
        return Failure(QStringLiteral("右键选择的路径不是可读文件夹。"));
    }
    if (rootInfo.isSymLink() || IsReparsePoint(rootInfo.absoluteFilePath()))
    {
        return Failure(QStringLiteral("不支持包含 Windows reparse point 或符号链接的文件夹。"));
    }

    std::uint64_t totalFileBytes = 0;
    std::uint64_t entryCount = 0;
    QDirIterator iterator(folderPath, QDir::AllEntries | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (iterator.hasNext())
    {
        const QString path = iterator.next();
        const QFileInfo info = iterator.fileInfo();
        entryCount++;
        if (entryCount > maximumArchiveEntries)
        {
            return Failure(QStringLiteral("文件夹条目数量超过有界上限。"));
        }
        if (info.isSymLink() || IsReparsePoint(path))
        {
            return Failure(QStringLiteral("不支持包含 Windows reparse point 或符号链接的文件夹。"));
        }
        if (info.isFile())
        {
            const qint64 fileBytes = info.size();
            if (fileBytes < 0)
            {
                return Failure(QStringLiteral("无法读取文件夹内文件大小。"));
            }
            std::uint64_t updatedBytes = 0;
            if (!AddChecked(totalFileBytes, static_cast<std::uint64_t>(fileBytes), updatedBytes) ||
                updatedBytes > maximumArchiveBytes)
            {
                return Failure(QStringLiteral("文件夹展开大小超过当前 500 GiB 有界上限。"));
            }
            totalFileBytes = updatedBytes;
        }
        else if (!info.isDir())
        {
            return Failure(QStringLiteral("文件夹包含不支持的文件系统对象。"));
        }
    }
    return {true, {}, {}};
}

[[nodiscard]] QString ResolveTarExecutable()
{
    const QString systemRoot = qEnvironmentVariable("SystemRoot");
    if (!systemRoot.isEmpty())
    {
        const QString systemTar = QDir(systemRoot).filePath(QStringLiteral("System32/tar.exe"));
        if (QFileInfo(systemTar).isFile())
        {
            return systemTar;
        }
    }
    return QStringLiteral("tar.exe");
}

} // namespace

FolderArchiveResult CreateFolderArchive(const QString& folderPath)
{
    const FolderArchiveResult treeStatus = ValidateFolderTree(folderPath);
    if (!treeStatus.success)
    {
        return treeStatus;
    }

    const QFileInfo rootInfo(folderPath);
    const QString absoluteFolderPath = QDir::cleanPath(rootInfo.absoluteFilePath());
    const QFileInfo absoluteInfo(absoluteFolderPath);
    const QString parentPath = absoluteInfo.dir().absolutePath();
    const QString leafName = absoluteInfo.fileName().isEmpty() ? QStringLiteral(".") : absoluteInfo.fileName();
    // Keep the generated source beside the durable Encoder session area so a
    // stopped folder transfer can be resumed without depending on the OS
    // temporary-file scavenger. The archive is still an ordinary local source
    // file; it never becomes a payload side channel.
    const QString archiveRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (archiveRoot.isEmpty())
    {
        return Failure(QStringLiteral("无法确定本地缓存目录，不能创建文件夹压缩包。"));
    }
    const QString archiveDirectory = QDir(archiveRoot).filePath(QStringLiteral("PixelBridgeEncoder/FolderArchives"));
    if (!QDir().mkpath(archiveDirectory))
    {
        return Failure(QStringLiteral("无法创建文件夹压缩包临时目录。"));
    }
    const QString archivePath = QDir(archiveDirectory).filePath(
        QStringLiteral("PixelBridgeFolder-%1.zip").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));

    QProcess process;
    process.setProgram(ResolveTarExecutable());
    process.setArguments({QStringLiteral("-caf"), archivePath, QStringLiteral("--directory"), parentPath, leafName});
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForStarted(5000) || !process.waitForFinished(-1) || process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0)
    {
        const QString detail = QString::fromLocal8Bit(process.readAll());
        QFile::remove(archivePath);
        return Failure(detail.isEmpty() ? QStringLiteral("Windows tar.exe 创建文件夹压缩包失败。") :
            QStringLiteral("Windows tar.exe 创建文件夹压缩包失败：%1").arg(detail.trimmed()));
    }

    const QFileInfo archiveInfo(archivePath);
    if (!archiveInfo.isFile() || archiveInfo.size() <= 0 ||
        static_cast<std::uint64_t>(archiveInfo.size()) > maximumArchiveBytes)
    {
        QFile::remove(archivePath);
        return Failure(QStringLiteral("文件夹压缩包超过当前 500 GiB 有界上限或为空。"));
    }
    return {true, archivePath, {}};
}

} // namespace pbencoder
