#include <git/mirror.h>
#include <git/ops.h>
#include <git/paths.h>
#include <osu/peek.h>
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
        const auto h = osu::peekOsuHeader(f.read(8192));
        if (!h) continue;
        if (h->beatmapId > 0 && !entry.beatmapIds.contains(h->beatmapId))
            entry.beatmapIds.append(h->beatmapId);
        if (entry.beatmapSetId <= 0 && h->beatmapSetId > 0)
            entry.beatmapSetId = h->beatmapSetId;
        if (entry.title.isEmpty()) {
            entry.title = h->title;
            entry.artist = h->artist;
            entry.creator = h->creator;
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

std::optional<SnapshotResult> snapshotMapset(const MapsetEntry& entry, const QString& trigger,
                                             const QMap<QString, QString>& extraTrailers,
                                             QString* err)
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
        difficulty = c.semantic->version;
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

    res.subject = triggerPrefix(trigger) + res.diff.subjectLine();
    res.commitOid = repo->commitStaged(*tree, res.subject, trailers, err);
    if (res.commitOid.isEmpty()) return std::nullopt;
    return res;
}

} // namespace ovc::git
