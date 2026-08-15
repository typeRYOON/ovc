#include <git/shadowrepo.h>
#include <watch/trackingservice.h>
#include <QDir>
#include <QFile>
#include <QtConcurrent>

namespace ovc::watch {

using ovc::git::MapsetEntry;
using ovc::git::Registry;

namespace {

// Submitting a set is the one event that changes an .osu without the mapper
// touching it: osu! stamps the assigned BeatmapID / BeatmapSetID into [Metadata]
// and renames the folder to "<setId> Artist - Title". Zeroing those two lines
// lets a pre-upload snapshot fingerprint identically to its just-submitted self,
// so the content probe still recognises the folder as the same tracked set.
QByteArray identityFingerprint(const QByteArray& osu)
{
    QByteArray out;
    out.reserve(osu.size());
    int i = 0;
    const int n = osu.size();
    while (i < n) {
        int eol = osu.indexOf('\n', i);
        if (eol < 0) eol = n;
        QByteArray line = osu.mid(i, eol - i); // without the '\n'
        const QByteArray key = line.trimmed();
        if (key.startsWith("BeatmapID:"))
            line = "BeatmapID:0";
        else if (key.startsWith("BeatmapSetID:"))
            line = "BeatmapSetID:0";
        out += line;
        if (eol < n) out += '\n';
        i = eol + 1;
    }
    return out;
}

} // namespace

TrackingService::TrackingService(QObject* parent)
    : QObject(parent), m_registry(Registry::load())
{
    // Freshen stored metadata from the .osu files on startup: picks up in-editor
    // title/artist edits and repairs any entry whose strings a legacy build stored
    // mis-encoded. Cheap (a header read per set); untouched folders leave values as-is.
    bool metaChanged = false;
    for (auto& e : m_registry.entries)
        if (ovc::git::refreshMapsetMetadata(e)) metaChanged = true;
    if (metaChanged) m_registry.save();

    connect(&m_binder, &SongsBinder::folderChangedStable, this,
            &TrackingService::onFolderChanged);
    connect(&m_snapWatcher, &QFutureWatcher<SnapJob>::finished, this, [this]() {
        const SnapJob job = m_snapWatcher.result();
        m_busy = false;
        if (job.res)
            emit snapshotTaken(job.repoId, job.res->subject, job.res->commitOid);
        else if (!job.err.isEmpty())
            emit snapshotFailed(job.repoId, job.err);
        else
            emit snapshotClean(job.repoId); // ran fine, nothing to commit
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
        // Content probe: does any tracked repo's HEAD hold an .osu whose identity
        // fingerprint matches one in this folder? Catches a plain folder rename
        // (bytes unchanged) AND an upload (folder renamed + IDs stamped into the
        // files) — the case where every other anchor moves at once.
        const QDir d(dir);
        const QStringList osuFiles = d.entryList({QStringLiteral("*.osu")}, QDir::Files);
        QList<QByteArray> diskPrints;
        for (const QString& name : osuFiles) {
            QFile f(d.filePath(name));
            if (f.open(QIODevice::ReadOnly))
                diskPrints.append(identityFingerprint(f.readAll()));
        }
        for (MapsetEntry& e : m_registry.entries) {
            auto repo = ovc::git::ShadowRepo::open(e.repoDir());
            if (!repo) continue;
            for (const auto& [relPath, blobOid] : repo->listTree(repo->headOid())) {
                if (!relPath.endsWith(QStringLiteral(".osu"))) continue;
                if (diskPrints.contains(identityFingerprint(repo->readBlob(blobOid)))) {
                    hit = &e;
                    break;
                }
            }
            if (hit) break;
        }
    }
    if (!hit) return {};

    // Keep the registry's notion of this set current: follow folder renames and
    // learn the real IDs the first time a submitted difficulty loads.
    bool dirty = false;
    bool listChanged = false;
    if (QDir::cleanPath(hit->songsPath) != dir) {
        hit->songsPath = dir;
        hit->folderName = QDir(dir).dirName();
        dirty = listChanged = true;
    }
    if (hit->beatmapSetId <= 0 && map.setId > 0) { // set uploaded since tracking
        hit->beatmapSetId = map.setId;
        dirty = true;
    }
    if (map.mapId > 0 && !hit->beatmapIds.contains(map.mapId)) { // diff got its ID
        hit->beatmapIds.append(map.mapId);
        dirty = true;
    }
    if (dirty) m_registry.save();
    if (listChanged) emit trackedListChanged();
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

bool TrackingService::untrack(const QString& repoId, bool deleteData, QString* err)
{
    const MapsetEntry* e = m_registry.findByRepoId(repoId);
    if (!e) {
        if (err) *err = QStringLiteral("not tracked");
        return false;
    }
    const QString dir = e->repoDir();
    const bool wasActive = (repoId == m_activeRepoId);
    m_registry.removeByRepoId(repoId);
    if (!m_registry.save(err)) return false;
    if (deleteData) QDir(dir).removeRecursively(); // the Songs folder is untouched
    if (wasActive) {
        m_activeRepoId.clear();
        m_binder.unbind();
        emit activeMapsetChanged(QString()); // the map may still be open, just untracked now
    }
    emit trackedListChanged();
    return true;
}

bool TrackingService::relink(const QString& repoId, const QString& newDir, QString* err)
{
    MapsetEntry* e = m_registry.findByRepoId(repoId);
    if (!e) {
        if (err) *err = QStringLiteral("not tracked");
        return false;
    }
    if (!ovc::git::relinkEntry(*e, newDir, err)) return false;
    if (!m_registry.save(err)) return false;
    // If this is the active set, follow it to the new folder so saves keep flowing.
    if (repoId == m_activeRepoId) {
        m_activeDir = QDir::cleanPath(newDir);
        m_binder.bind(m_activeDir);
    }
    emit trackedListChanged();
    return true;
}

void TrackingService::onFolderChanged()
{
    if (m_activeRepoId.isEmpty()) return;
    const MapsetEntry* e = m_registry.findByRepoId(m_activeRepoId);
    if (!e || !e->autoSnapshot) return;
    enqueueSnapshot(m_activeRepoId, QStringLiteral("autosave"));
}

void TrackingService::requestManualSnapshot(const QString& repoId, const QString& name)
{
    if (!name.trimmed().isEmpty()) m_pendingName[repoId] = name.trimmed();
    enqueueSnapshot(repoId, QStringLiteral("manual"));
}

QString TrackingService::takePendingName(const QString& repoId)
{
    return m_pendingName.take(repoId);
}

void TrackingService::setAutoSnapshot(const QString& repoId, bool on)
{
    MapsetEntry* e = m_registry.findByRepoId(repoId);
    if (!e || e->autoSnapshot == on) return;
    e->autoSnapshot = on;
    m_registry.save();
    emit trackedListChanged();
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

    // A name given at creation becomes the snapshot's label once it commits.
    const QString name = takePendingName(repoId);

    m_busy = true;
    const MapsetEntry entry = *e; // worker gets a copy; registry stays GUI-thread-only
    m_snapWatcher.setFuture(QtConcurrent::run([entry, trigger, trailers, name]() {
        SnapJob job;
        job.repoId = entry.repoId;
        job.res = ovc::git::snapshotMapset(entry, trigger, trailers, &job.err);
        if (job.res && !name.isEmpty()) {
            if (auto repo = ovc::git::ShadowRepo::open(entry.repoDir()))
                repo->setLabel(job.res->commitOid, name);
        }
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
