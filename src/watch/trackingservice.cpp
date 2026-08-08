#include <git/shadowrepo.h>
#include <watch/trackingservice.h>
#include <QDir>
#include <QFile>
#include <QtConcurrent>

namespace ovc::watch {

using ovc::git::MapsetEntry;
using ovc::git::Registry;

TrackingService::TrackingService(QObject* parent)
    : QObject(parent), m_registry(Registry::load())
{
    connect(&m_binder, &SongsBinder::folderChangedStable, this,
            &TrackingService::onFolderChanged);
    connect(&m_snapWatcher, &QFutureWatcher<SnapJob>::finished, this, [this]() {
        const SnapJob job = m_snapWatcher.result();
        m_busy = false;
        if (job.res)
            emit snapshotTaken(job.repoId, job.res->subject, job.res->commitOid);
        else if (!job.err.isEmpty())
            emit snapshotFailed(job.repoId, job.err);
        runNextSnapshot();
    });
}

void TrackingService::setEditorTimeProvider(std::function<int()> provider)
{
    m_editorTime = std::move(provider);
}

QString TrackingService::activeDir() const
{
    if (m_current.songsDir.isEmpty() || m_current.folder.isEmpty()) return {};
    return QDir::cleanPath(m_current.songsDir + QLatin1Char('/') + m_current.folder);
}

QString TrackingService::resolveIdentity(const MemBeatmap& map)
{
    const QString dir = QDir::cleanPath(map.songsDir + QLatin1Char('/') + map.folder);

    MapsetEntry* hit = m_registry.findBySetId(map.setId);
    if (!hit) hit = m_registry.findBySongsPath(dir);
    if (!hit) hit = m_registry.findByBeatmapId(map.mapId);

    if (!hit) {
        // Content probe: does any tracked repo's HEAD hold a byte-identical
        // .osu from this folder? Catches folder renames of unsubmitted sets.
        const QDir d(dir);
        const QStringList osuFiles = d.entryList({QStringLiteral("*.osu")}, QDir::Files);
        for (MapsetEntry& e : m_registry.entries) {
            auto repo = ovc::git::ShadowRepo::open(e.repoDir());
            if (!repo) continue;
            for (const auto& [relPath, blobOid] : repo->listTree(repo->headOid())) {
                if (!relPath.endsWith(QStringLiteral(".osu"))) continue;
                for (const QString& name : osuFiles) {
                    QFile f(d.filePath(name));
                    if (f.open(QIODevice::ReadOnly) && f.readAll() == repo->readBlob(blobOid)) {
                        hit = &e;
                        break;
                    }
                }
                if (hit) break;
            }
            if (hit) break;
        }
    }
    if (!hit) return {};

    // Keep the registry's notion of where the folder lives current.
    if (QDir::cleanPath(hit->songsPath) != dir) {
        hit->songsPath = dir;
        hit->folderName = QDir(dir).dirName();
        m_registry.save();
        emit trackedListChanged();
    }
    if (hit->beatmapSetId <= 0 && map.setId > 0) { // set uploaded since tracking
        hit->beatmapSetId = map.setId;
        m_registry.save();
    }
    return hit->repoId;
}

void TrackingService::onBeatmapChanged(const MemBeatmap& map)
{
    m_current = map;
    const QString rid = resolveIdentity(map);
    const QString dir = activeDir();

    if (rid == m_activeRepoId && dir == m_activeDir) {
        // Same mapset (e.g. difficulty switch) — just keep the watch alive.
        if (!rid.isEmpty() && m_binder.boundDir() != dir) m_binder.bind(dir);
        return;
    }

    // Switching maps mid-debounce: the old folder's last save still counts.
    if (m_binder.flushPending() && !m_activeRepoId.isEmpty())
        enqueueSnapshot(m_activeRepoId, QStringLiteral("autosave"));

    // A tracked set whose folder merely moved rebinds silently
    // (trackedListChanged already fired); anything else notifies the UI.
    const bool notify = rid != m_activeRepoId || rid.isEmpty();
    m_activeRepoId = rid;
    m_activeDir = dir;
    if (rid.isEmpty())
        m_binder.unbind();
    else
        m_binder.bind(dir);
    if (notify) emit activeMapsetChanged(rid);
}

void TrackingService::onBeatmapCleared()
{
    if (m_binder.flushPending() && !m_activeRepoId.isEmpty())
        enqueueSnapshot(m_activeRepoId, QStringLiteral("autosave"));
    m_current = MemBeatmap{};
    m_binder.unbind();
    const bool hadSomething = !m_activeRepoId.isEmpty() || !m_activeDir.isEmpty();
    m_activeRepoId.clear();
    m_activeDir.clear();
    if (hadSomething) emit activeMapsetChanged({});
}

void TrackingService::onStateChanged(GameState state)
{
    m_state = state;
}

std::optional<MapsetEntry> TrackingService::trackCurrentMapset(QString* err)
{
    const QString dir = activeDir();
    if (dir.isEmpty()) {
        if (err) *err = QStringLiteral("no beatmap detected");
        return std::nullopt;
    }
    const auto entry = ovc::git::trackMapset(dir, err);
    if (!entry) return std::nullopt;
    m_registry = Registry::load();
    m_activeRepoId = entry->repoId;
    m_activeDir = dir;
    m_binder.bind(dir);
    emit trackedListChanged();
    emit activeMapsetChanged(entry->repoId);
    return entry;
}

void TrackingService::onFolderChanged()
{
    if (m_activeRepoId.isEmpty()) return;
    const MapsetEntry* e = m_registry.findByRepoId(m_activeRepoId);
    if (!e || !e->autoSnapshot) return;
    enqueueSnapshot(m_activeRepoId, QStringLiteral("autosave"));
}

void TrackingService::requestManualSnapshot(const QString& repoId)
{
    enqueueSnapshot(repoId, QStringLiteral("manual"));
}

void TrackingService::enqueueSnapshot(const QString& repoId, const QString& trigger)
{
    if (repoId.isEmpty()) return;
    // Manual outranks a queued autosave; never the other way around.
    if (m_pending.value(repoId) != QStringLiteral("manual")) m_pending[repoId] = trigger;
    runNextSnapshot();
}

void TrackingService::runNextSnapshot()
{
    if (m_busy || m_pending.isEmpty()) return;
    const QString repoId = m_pending.constBegin().key();
    const QString trigger = m_pending.take(repoId);
    const MapsetEntry* e = m_registry.findByRepoId(repoId);
    if (!e) return;

    QMap<QString, QString> trailers;
    if (m_state == GameState::Edit && m_editorTime) {
        const int t = m_editorTime();
        if (t >= 0) trailers.insert(QStringLiteral("Ovc-Editor-Time"), QString::number(t));
    }

    m_busy = true;
    const MapsetEntry entry = *e; // worker gets a copy; registry stays GUI-thread-only
    m_snapWatcher.setFuture(QtConcurrent::run([entry, trigger, trailers]() {
        SnapJob job;
        job.repoId = entry.repoId;
        job.res = ovc::git::snapshotMapset(entry, trigger, trailers, &job.err);
        return job;
    }));
}

TrackingService::RestorePreflight TrackingService::preflightRestore(const QString& repoId) const
{
    if (m_state == GameState::Edit && repoId == m_activeRepoId && !repoId.isEmpty()) {
        return {false,
                tr("Close the editor (or switch songs) first — osu! would overwrite the "
                   "restored files on its next save.")};
    }
    return {true, {}};
}

std::optional<ovc::git::SnapshotResult> TrackingService::restore(const QString& repoId,
                                                                 const QByteArray& oid,
                                                                 QString* err)
{
    const auto pf = preflightRestore(repoId);
    if (!pf.allowed) {
        if (err) *err = pf.reason;
        return std::nullopt;
    }
    const MapsetEntry* e = m_registry.findByRepoId(repoId);
    if (!e) {
        if (err) *err = QStringLiteral("unknown repo ") + repoId;
        return std::nullopt;
    }
    const auto res = ovc::git::restoreMapset(*e, oid, err);
    if (res) emit snapshotTaken(repoId, res->subject, res->commitOid);
    return res;
}

} // namespace ovc::watch
