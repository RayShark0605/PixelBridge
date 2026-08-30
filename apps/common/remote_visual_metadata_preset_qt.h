#pragma once

#include "application_model.h"

#include <QString>

namespace pbapp
{

// Qt application-layer parser for the versioned experiment preset. The
// resulting metadata is evidence only; production wire and acceptance code do
// not consume it.
[[nodiscard]] bool LoadRemoteVisualMetadataPreset(const QString& path, RemoteRunMetadata& output,
    QString& errorMessage);

} // namespace pbapp
