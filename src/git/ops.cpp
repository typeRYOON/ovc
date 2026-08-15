#include <git/bundle.h>
#include <git/mirror.h>
#include <git/ops.h>
#include <git/paths.h>
#include <ovccore/peek.h>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace ovc::git {

namespace {

QString triggerPrefix(const QString& trigger)
{
    if (trigger == QStringLiteral("autosave")) return QStringLiteral("[auto] ");
    return QLatin1Char('[') + trigger + QStringLiteral("] ");
}

} // namespace

std::optional<MapsetEntry> trackMapset(const QString& songsDir, QString* err)
{
    const QString clean = QDir::cleanPath(songsDir);
    const QDir dir(clean);
    if (!dir.exists()) {
        if (err) *err = QStringLiteral("folder does not exist: ") + clean;
        return std::nullopt;
    }

    Registry reg = Registry::load();
    if (reg.findBySongsPath(clean)) {
        if (err) *err = QStringLiteral("already tracked: ") + clean;
        return std::nullopt;
    }

    MapsetEntry entry;
    entry.songsPath = clean;
    entry.folderName = dir.dirName();
    entry.trackedSince = QDateTime::currentDateTimeUtc();

    // Identity from the difficulties (.osu files live at the mapset root).
    int fileCount = 0;
    qint64 byteCount = 0;
    QDirIterator all(clean, QDir::Files, QDirIterator::Subdirectories);
    while (all.hasNext()) {
        all.next();
        ++fileCount;
        byteCount += all.fileInfo().size();
    }
    const QStringList osuFiles = dir.entryList({QStringLiteral("*.osu")}, QDir::Files);
    for (const QString& name : osuFiles) {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray head = f.read(8192);
        const auto h = core::peekOsuHeader({head.constData(), size_t(head.size())});
        if (!h) continue;
        if (h->beatmapId > 0 && !entry.beatmapIds.contains(h->beatmapId))
            entry.beatmapIds.append(h->beatmapId);
        if (entry.beatmapSetId <= 0 && h->beatmapSetId > 0)
            entry.beatmapSetId = h->beatmapSetId;
        if (entry.title.isEmpty()) {
            entry.title = QString::fromStdString(h->title);
            entry.artist = QString::fromStdString(h->artist);
            entry.creator = QString::fromStdString(h->creator);
        }
    }
    if (osuFiles.isEmpty()) {
        if (err) *err = QStringLiteral("no .osu files in ") + clean;
        return std::nullopt;
    }

    entry.repoId = reg.newRepoId();
    const QString repoDir = entry.repoDir();
    if (!ShadowRepo::create(repoDir, err)) return std::nullopt;

    // Committed identity record (shared with collaborators at the sync milestone).
    QDir().mkpath(repoDir + QStringLiteral("/.ovc"));
    QJsonArray ids;
    for (int id : entry.beatmapIds) ids.append(id);
    QFile mapsetJson(repoDir + QStringLiteral("/.ovc/mapset.json"));
    if (mapsetJson.open(QIODevice::WriteOnly)) { // closed below before staging reads it
        mapsetJson.write(QJsonDocument(QJsonObject{
                                           {QStringLiteral("beatmapSetId"), entry.beatmapSetId},
                                           {QStringLiteral("beatmapIds"), ids},
                                           {QStringLiteral("folderName"), entry.folderName},
                                           {QStringLiteral("title"), entry.title},
                                           {QStringLiteral("artist"), entry.artist},
                                           {QStringLiteral("creator"), entry.creator},
                                       })
                             .toJson());
        mapsetJson.close();
    }

    if (!mirrorIntoRepo(clean, repoDir, nullptr, err)) return std::nullopt;

    auto repo = ShadowRepo::open(repoDir);
    if (!repo) {
        if (err) *err = QStringLiteral("cannot open repo ") + repoDir;
        return std::nullopt;
    }
    const QString subject =
        QStringLiteral("[import] Initial snapshot (%1 files, %2 MiB)")
            .arg(fileCount)
            .arg(QString::number(double(byteCount) / (1024.0 * 1024.0), 'f', 1));
    if (!repo->commitAll(subject, {{QStringLiteral("Ovc-Trigger"), QStringLiteral("import")}},
                         err)) {
        return std::nullopt;
    }

    reg.entries.append(entry);
    if (!reg.save(err)) return std::nullopt;
    return entry;
}

bool refreshMapsetMetadata(MapsetEntry& entry)
{
    const QDir dir(entry.songsPath);
    if (!dir.exists()) return false; // folder gone: keep whatever we had

    QString title, artist, creator;
    const QStringList osuFiles = dir.entryList({QStringLiteral("*.osu")}, QDir::Files);
    for (const QString& name : osuFiles) {
        if (!title.isEmpty() && !artist.isEmpty() && !creator.isEmpty()) break;
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray head = f.read(8192);
        const auto h = core::peekOsuHeader({head.constData(), size_t(head.size())});
        if (!h) continue;
        if (title.isEmpty() && !h->title.empty()) title = QString::fromStdString(h->title);
        if (artist.isEmpty() && !h->artist.empty()) artist = QString::fromStdString(h->artist);
        if (creator.isEmpty() && !h->creator.empty()) creator = QString::fromStdString(h->creator);
    }

    bool changed = false;
    if (!title.isEmpty() && entry.title != title) { entry.title = title; changed = true; }
    if (!artist.isEmpty() && entry.artist != artist) { entry.artist = artist; changed = true; }
    if (!creator.isEmpty() && entry.creator != creator) { entry.creator = creator; changed = true; }
    return changed;
}

bool relinkEntry(MapsetEntry& entry, const QString& newSongsDir, QString* err)
{
    const QString clean = QDir::cleanPath(newSongsDir);
    const QDir dir(clean);
    if (!dir.exists()) {
        if (err) *err = QStringLiteral("folder does not exist: ") + clean;
        return false;
    }
    const QStringList osuFiles = dir.entryList({QStringLiteral("*.osu")}, QDir::Files);
    if (osuFiles.isEmpty()) {
        if (err) *err = QStringLiteral("no .osu files in ") + clean;
        return false;
    }
    entry.songsPath = clean;
    entry.folderName = dir.dirName();
    // Refresh identity from the (now-submitted) difficulties. beatmapIds
    // accumulate; setId is overwritten since a submitted set carries the real one.
    for (const QString& name : osuFiles) {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray head = f.read(8192);
        const auto h = core::peekOsuHeader({head.constData(), size_t(head.size())});
        if (!h) continue;
        if (h->beatmapId > 0 && !entry.beatmapIds.contains(h->beatmapId))
            entry.beatmapIds.append(h->beatmapId);
        if (h->beatmapSetId > 0) entry.beatmapSetId = h->beatmapSetId;
    }
    return true;
}

std::optional<SnapshotResult> snapshotMapset(const MapsetEntry& entry, const QString& trigger,
                                             const QMap<QString, QString>& extraTrailers,
                                             QString* err, const QString& subjectOverride)
{
    if (err) err->clear();
    auto repo = ShadowRepo::open(entry.repoDir());
    if (!repo) {
        if (err) *err = QStringLiteral("repo missing: ") + entry.repoDir();
        return std::nullopt;
    }

    if (!mirrorIntoRepo(entry.songsPath, entry.repoDir(), nullptr, err)) return std::nullopt;

    const QByteArray parent = repo->headOid();
    const auto tree = repo->stageAll(err);
    if (!tree) return std::nullopt; // clean when err stayed empty

    SnapshotResult res;
    res.diff = diffTrees(*repo, parent, *tree);

    QMap<QString, QString> trailers = extraTrailers;
    trailers.insert(QStringLiteral("Ovc-Trigger"), trigger);
    trailers.insert(QStringLiteral("Ovc-Files"), QString::number(res.diff.files.size()));

    // Difficulty + time-range trailers from the semantic diffs.
    QString difficulty;
    int diffCount = 0;
    int lo = INT_MAX, hi = INT_MIN;
    for (const FileChange& c : res.diff.files) {
        if (c.kind != FileKind::Difficulty || !c.semantic) continue;
        ++diffCount;
        difficulty = QString::fromStdString(c.semantic->version);
        const auto range = c.semantic->affectedTimeRange();
        if (range.first >= 0) {
            lo = std::min(lo, range.first);
            hi = std::max(hi, range.second);
        }
    }
    if (diffCount == 1 && !difficulty.isEmpty())
        trailers.insert(QStringLiteral("Ovc-Difficulty"), difficulty);
    if (lo != INT_MAX)
        trailers.insert(QStringLiteral("Ovc-Time-Range"),
                        QStringLiteral("%1-%2").arg(lo).arg(hi));

    res.subject = subjectOverride.isEmpty() ? triggerPrefix(trigger) + res.diff.subjectLine()
                                            : subjectOverride;
    res.commitOid = repo->commitStaged(*tree, res.subject, trailers, err);
    if (res.commitOid.isEmpty()) return std::nullopt;
    return res;
}

std::optional<SnapshotResult> restoreMapset(const MapsetEntry& entry, const QByteArray& commitOid,
                                            QString* err)
{
    if (err) err->clear();
    if (entry.songsPath.isEmpty()) {
        // View-only archives (bundle imports without --into) have no target.
        if (err) *err = QStringLiteral("no local folder linked to this mapset");
        return std::nullopt;
    }
    auto repo = ShadowRepo::open(entry.repoDir());
    if (!repo) {
        if (err) *err = QStringLiteral("repo missing: ") + entry.repoDir();
        return std::nullopt;
    }
    const auto targetInfo = repo->commitInfo(commitOid);
    if (!targetInfo) {
        if (err) *err = QStringLiteral("unknown commit ") + commitOid;
        return std::nullopt;
    }

    auto isInfra = [](const QString& p) {
        return p == QStringLiteral(".gitattributes") || p.startsWith(QStringLiteral(".ovc/"));
    };

    // Safety net: whatever is in Songs right now gets its own commit first.
    // A missing/empty folder (bundle import restoring into a fresh target)
    // has nothing worth saving — snapshotting it would commit an empty tree.
    QDir().mkpath(entry.songsPath);
    const bool songsHasContent =
        !QDir(entry.songsPath)
             .entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)
             .isEmpty();
    if (songsHasContent) {
        snapshotMapset(entry, QStringLiteral("pre-restore"), {}, err);
        if (err && !err->isEmpty()) return std::nullopt;
    }

    QSet<QString> targetPaths;
    for (const auto& [relPath, blobOid] : repo->listTree(commitOid)) {
        if (isInfra(relPath)) continue; // repo metadata never materializes Songs-side
        targetPaths.insert(relPath);
        const QString dest = entry.songsPath + QLatin1Char('/') + relPath;
        QDir().mkpath(QFileInfo(dest).absolutePath());
        const QString tmp = dest + QStringLiteral(".ovctmp");
        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly) || f.write(repo->readBlob(blobOid)) < 0) {
            if (err) *err = QStringLiteral("cannot write ") + tmp;
            return std::nullopt;
        }
        f.close();
        if (QFile::exists(dest)) QFile::remove(dest);
        if (!QFile::rename(tmp, dest)) {
            if (err) *err = QStringLiteral("cannot replace ") + dest;
            return std::nullopt;
        }
    }

    // Files the repo tracks now but the target lacks: remove from Songs.
    // Anything the repo has never seen stays untouched.
    for (const auto& [relPath, blobOid] : repo->listTree(repo->headOid())) {
        Q_UNUSED(blobOid)
        if (isInfra(relPath) || targetPaths.contains(relPath)) continue;
        QFile::remove(entry.songsPath + QLatin1Char('/') + relPath);
    }

    const QString subject =
        QStringLiteral("[restore] State of %1 (%2)")
            .arg(targetInfo->when.toLocalTime().toString(QStringLiteral("MMM d, HH:mm")),
                 QString::fromUtf8(commitOid.left(7)));
    return snapshotMapset(entry, QStringLiteral("restore"),
                          {{QStringLiteral("Ovc-Restored-From"),
                            QString::fromUtf8(commitOid.left(7))}},
                          err, subject);
}

std::optional<CollabMergeOutcome> applyBundleMerge(const MapsetEntry& entry,
                                                   const PreparedMerge& prepared,
                                                   const FileResolutions& resolutions,
                                                   QString* err)
{
    if (err) err->clear();
    if (entry.songsPath.isEmpty()) {
        if (err) *err = QStringLiteral("no local folder linked to this mapset");
        return std::nullopt;
    }

    // Safety net: capture whatever is in Songs now so the merge is reversible.
    snapshotMapset(entry, QStringLiteral("pre-merge"), {}, err);
    if (err && !err->isEmpty()) return std::nullopt;

    const BundleMergeReport report = applyPreparedMerge(entry, prepared, resolutions, err);
    if (err && !err->isEmpty()) return std::nullopt;

    CollabMergeOutcome out;
    out.report = report;
    if (!report.anyChange()) return out; // nothing new to write

    const int conflicts = report.totalConflicts();
    QStringList bits;
    if (conflicts == 0) bits << QStringLiteral("clean");
    else if (!resolutions.isEmpty()) bits << QStringLiteral("resolved %1").arg(conflicts);
    else bits << QStringLiteral("%1 conflict%2, kept ours").arg(conflicts).arg(conflicts == 1 ? "" : "s");
    if (const int mw = report.mediaWritten()) bits << QStringLiteral("+%1 media").arg(mw);
    if (const int mk = report.mediaKeptOurs()) bits << QStringLiteral("%1 media kept").arg(mk);
    const QString subject =
        QStringLiteral("[merge] %1 (%2)").arg(report.bundleTitle, bits.join(QStringLiteral(", ")));
    const auto res = snapshotMapset(entry, QStringLiteral("merge"),
                                    {{QStringLiteral("Ovc-Merge-From"), report.bundleTitle}}, err,
                                    subject);
    if (res) out.snapshotOid = res->commitOid;
    return out;
}

std::optional<CollabMergeOutcome> collabMergeBundle(const MapsetEntry& entry,
                                                    const QString& bundlePath, QString* err)
{
    const auto prepared = prepareBundleMerge(entry, bundlePath, err);
    if (!prepared) return std::nullopt;
    return applyBundleMerge(entry, *prepared, {}, err); // ours wins any conflict
}

} // namespace ovc::git
