#pragma once

#include <QString>

namespace pbencoder
{

struct FolderArchiveResult
{
    bool success = false;
    QString archivePath;
    QString errorMessage;
};

[[nodiscard]] FolderArchiveResult CreateFolderArchive(const QString& folderPath);

} // namespace pbencoder
