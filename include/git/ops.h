#pragma once
#include <git/registry.h>
#include <git/setdiff.h>
#include <QMap>
#include <optional>

namespace ovc::git {

// High-level operations shared by ovc-cli and the tracking service.

struct SnapshotResult {
    QByteArray commitOid;
    SetDiff diff;
    QString subject;
};

// Import a mapset folder: new registry entry + repo + initial commit.
std::optional<MapsetEntry> trackMapset(const QString& songsDir, QString* err);

// Mirror + commit if anything changed. nullopt with empty err = clean no-op.
// `trigger` lands in the subject prefix and the Ovc-Trigger trailer
// (autosave | manual | import | pre-restore | restore).
std::optional<SnapshotResult> snapshotMapset(const MapsetEntry& entry, const QString& trigger,
                                             const QMap<QString, QString>& extraTrailers,
                                             QString* err);

} // namespace ovc::git
