#pragma once
#include <git/registry.h>
#include <QString>
#include <optional>

namespace ovc::git {

// .ovcz — a mapset's full snapshot history as one shareable file (zip:
// bundle.json + content-addressed blobs). Nothing ever touches a server;
// the web viewer reads these client-side and friends exchange them directly.
//
// textOnly: media entries stay listed in the trees (path + size + hash) but
// their bytes are omitted — semantic .osu history intact at a fraction of
// the size.

struct BundleInfo {
    QString title, artist, creator;
    int snapshotCount = 0;
    bool textOnly = false;
};

bool exportBundle(const MapsetEntry& entry, const QString& outPath, bool textOnly,
                  QString* err = nullptr);

// Creates a NEW tracked entry replaying the bundle's history (original oids
// preserved as Ovc-Bundle-Oid trailers). `intoSongsFolder` optionally links a
// local folder so restore works; empty = view-only archive.
std::optional<MapsetEntry> importBundle(const QString& bundlePath,
                                        const QString& intoSongsFolder = {},
                                        QString* err = nullptr);

std::optional<BundleInfo> peekBundle(const QString& bundlePath, QString* err = nullptr);

// ---- collab merge -----------------------------------------------------------

// One difficulty's merge outcome.
struct FileMergeResult {
    QString relPath;
    QString version;
    bool wholeFileConflict = false; // mode / key count differ — left ours alone
    QString reason;
    QStringList conflictKeys; // human keys of overlapping edits (ours kept)
    bool changed = false;     // the merge altered ours' file
    int conflictCount() const { return int(conflictKeys.size()); }
};

// One media (non-.osu) file's sync outcome. Binary files can't be 3-way merged,
// so each is a whole-file decision keyed on content hash.
struct MediaMergeResult {
    enum class Op { Added, Updated, KeptOurs, Skipped };
    QString relPath;
    Op op = Op::Added;
    qint64 size = 0;  // bytes written (Added/Updated), else theirs' size
    QString reason;   // Skipped only (e.g. "text-only bundle")
};

struct BundleMergeReport {
    QString bundleTitle;
    QList<FileMergeResult> files;   // only difficulties that differed
    QList<MediaMergeResult> media;  // media pulled in / kept / skipped
    int totalConflicts() const;     // difficulty conflicts (interactive)
    int mediaWritten() const;       // media Added + Updated
    int mediaKeptOurs() const;      // binary conflicts left as ours
    bool anyChange() const;
};

// One overlapping edit, for the website resolver. `id` maps a chosen side back
// to the exact conflict when applying.
struct MergeConflictInfo {
    QString id;      // stable core Conflict id
    QString domain;  // "kv" | "note" | "timing" | "break" | "storyboard"
    QString section; // for kv
    QString key;     // human label
    int timeMs = -1;
    int column = -1;
    QString base, ours, theirs; // rendered values ("" = absent that side)
};

// A merge computed but not yet written — held pending until conflicts are
// resolved (in the web viewer). Carries the base/ours/theirs text per file so
// it can be re-merged with the user's per-conflict choices.
struct PreparedFile {
    QString relPath;
    QString version;
    bool addedByThem = false;    // a difficulty they added; theirsText is the whole content
    bool wholeFileConflict = false;
    QString reason;
    int mode = -1;      // game mode (0 std, 1 taiko, 2 catch, 3 mania) — for the web resolver
    int keyCount = 0;   // mania column count (0 otherwise)
    QByteArray baseText, oursText, theirsText;
    QList<MergeConflictInfo> conflicts;
};

// A media file the collaborator has that we may want. Bytes are read from the
// bundle at apply time (media can be large — we hold only the blob oid).
struct PreparedMedia {
    // Add: ours lacks it. Update: theirs changed, ours didn't → take theirs.
    // Conflict: both changed the same file → kept ours (resolvable via id
    // "media:<relPath>" → theirs; defaults to ours).
    enum class Op { Add, Update, Conflict };
    QString relPath;
    QByteArray theirsBlob; // oid under the bundle's blobs/
    qint64 theirsSize = 0;
    Op op = Op::Add;
};

struct PreparedMerge {
    QString repoId;
    QString bundleTitle;
    QString bundlePath;
    QList<PreparedFile> files;   // only differing or newly-added difficulties
    QList<PreparedMedia> media;  // media to add / update / (conflict) from theirs
    int totalConflicts() const;
    bool hasConflicts() const;   // difficulty conflicts only — media never parks the merge
    bool anyMergeable() const;   // any file we'd actually write
};

// Compute a merge without touching disk. base of each difficulty = newest .osu
// blob common to both histories (content-addressed); media the collaborator
// added or updated (and ours didn't) is queued to copy in, binary conflicts
// kept ours. Media is skipped for text-only bundles (no bytes to copy).
std::optional<PreparedMerge> prepareBundleMerge(const MapsetEntry& ours, const QString& bundlePath,
                                                QString* err);

// Per-file merge resolutions: relPath → (conflict id → "ours"|"theirs"). Scoped
// per file because conflict ids (kv:…, note:…, "storyboard", …) repeat across a
// mapset's difficulties and must be resolvable independently.
using FileResolutions = QMap<QString, QMap<QString, QString>>;

// Apply a prepared merge into ours' Songs folder. `resolutions` is per-file (see
// FileResolutions); anything absent stays ours. Does NOT snapshot — callers wrap
// with pre/post snapshots. Returns the per-file outcome.
BundleMergeReport applyPreparedMerge(const MapsetEntry& ours, const PreparedMerge& prepared,
                                     const FileResolutions& resolutions, QString* err);

} // namespace ovc::git
