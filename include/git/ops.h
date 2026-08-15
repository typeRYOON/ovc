#pragma once
#include <git/bundle.h>
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

// Re-read title/artist/creator from the tracked .osu files into `entry`. Keeps
// the tracked list in step with in-editor metadata edits, and self-heals entries
// whose stored strings were mangled by an older build. Missing/unreadable folders
// leave the stored values untouched. Returns true if anything changed. Does NOT
// persist; the caller owns the registry save.
bool refreshMapsetMetadata(MapsetEntry& entry);

// Re-point an existing tracked entry at a new Songs folder — the manual escape
// hatch for when an upload renamed the folder and stamped IDs into the .osu, so
// auto-detection saw a stranger. Updates songsPath/folderName and refreshes
// beatmapSetId / beatmapIds from the new .osu files. Does NOT persist; the
// caller owns the registry save.
bool relinkEntry(MapsetEntry& entry, const QString& newSongsDir, QString* err);

// Mirror + commit if anything changed. nullopt with empty err = clean no-op.
// `trigger` lands in the subject prefix and the Ovc-Trigger trailer
// (autosave | manual | import | pre-restore | restore).
std::optional<SnapshotResult> snapshotMapset(const MapsetEntry& entry, const QString& trigger,
                                             const QMap<QString, QString>& extraTrailers,
                                             QString* err, const QString& subjectOverride = {});

// Write a commit's tree back into the Songs folder (tmp+replace per file,
// tracked-but-absent files deleted, unknown junk left alone), after a
// pre-restore safety snapshot if dirty. The restored state lands as a NEW
// commit. nullopt with empty err = Songs already matched that commit.
std::optional<SnapshotResult> restoreMapset(const MapsetEntry& entry, const QByteArray& commitOid,
                                            QString* err);

// Collab: merge a collaborator's bundle into `entry`. Snapshots ours first
// (safety), runs the per-difficulty 3-way merge into the Songs folder, then
// snapshots the merged result (trigger "merge"). Returns the merge report;
// `snapshotOid` on it (via the report) plus conflict details drive the UI.
struct CollabMergeOutcome {
    BundleMergeReport report;
    QByteArray snapshotOid; // the merge snapshot, empty if nothing changed
};
std::optional<CollabMergeOutcome> collabMergeBundle(const MapsetEntry& entry,
                                                    const QString& bundlePath, QString* err);

// Apply an already-prepared merge with the user's per-conflict resolutions
// (used by the web-resolver flow). Safety snapshot → write → merge snapshot.
std::optional<CollabMergeOutcome> applyBundleMerge(const MapsetEntry& entry,
                                                   const PreparedMerge& prepared,
                                                   const FileResolutions& resolutions,
                                                   QString* err);

} // namespace ovc::git
