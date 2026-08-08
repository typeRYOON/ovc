#include <watch/songsbinder.h>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

namespace ovc::watch {

// Save-behavior spike findings (stable, 2026-08-08, ReadDirectoryChangesW):
// a save is REMOVED → ADDED → MODIFIED of the .osu within ≤10ms — the editor
// deletes and recreates the file, never temp+rename, never in-place truncate.
// Only artist/title/creator/version edits rename the file (tags don't).
// Fastest observed double-save was 676ms apart; the 800ms debounce coalesces
// those and splits saves ≥2s apart into their own snapshots.
namespace {
constexpr int kMaxSubdirs = 64;
constexpr int kMaxFileWatches = 256;
constexpr int kMaxStabilityTries = 40; // give up waiting after ~6s of churn
} // namespace

SongsBinder::SongsBinder(QObject* parent) : QObject(parent)
{
    m_debounce.setSingleShot(true);
    m_stability.setSingleShot(true);
    // Directory events cover add/remove/rename; they do NOT cover in-place
    // content rewrites on Windows (NTFS leaves the dir entry untouched), so
    // the files themselves are watched as well.
    connect(&m_fsw, &QFileSystemWatcher::directoryChanged, this, &SongsBinder::onDirEvent);
    connect(&m_fsw, &QFileSystemWatcher::fileChanged, this, &SongsBinder::onDirEvent);
    connect(&m_debounce, &QTimer::timeout, this, &SongsBinder::onDebounce);
    connect(&m_stability, &QTimer::timeout, this, &SongsBinder::onStabilityTick);
}

void SongsBinder::setTimings(int debounceMs, int stabilityMs)
{
    m_debounceMs = debounceMs;
    m_stabilityMs = stabilityMs;
}

void SongsBinder::bind(const QString& mapsetDir)
{
    unbind();
    m_dir = QDir::cleanPath(mapsetDir);
    addWatchPaths();
}

void SongsBinder::unbind()
{
    m_debounce.stop();
    m_stability.stop();
    if (!m_fsw.directories().isEmpty()) m_fsw.removePaths(m_fsw.directories());
    if (!m_fsw.files().isEmpty()) m_fsw.removePaths(m_fsw.files());
    m_dir.clear();
}

bool SongsBinder::flushPending()
{
    if (!m_debounce.isActive() && !m_stability.isActive()) return false;
    m_debounce.stop();
    m_stability.stop();
    emit folderChangedStable();
    return true;
}

void SongsBinder::addWatchPaths()
{
    if (m_dir.isEmpty() || !QDir(m_dir).exists()) return;

    QStringList wantDirs{m_dir};
    QDirIterator dirs(m_dir, QDir::Dirs | QDir::NoDotAndDotDot);
    while (dirs.hasNext() && wantDirs.size() < kMaxSubdirs) wantDirs << dirs.next();

    // Saved text first (.osu/.osb — the files that change every save), then
    // whatever media fits under the cap.
    QStringList wantFiles, media;
    QDirIterator files(m_dir, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString f = files.next();
        if (f.endsWith(QStringLiteral(".osu"), Qt::CaseInsensitive) ||
            f.endsWith(QStringLiteral(".osb"), Qt::CaseInsensitive))
            wantFiles << f;
        else
            media << f;
    }
    for (const QString& f : media) {
        if (wantFiles.size() >= kMaxFileWatches) break;
        wantFiles << f;
    }

    const QStringList curDirs = m_fsw.directories();
    const QStringList curFiles = m_fsw.files();
    QStringList missing;
    for (const QString& w : wantDirs)
        if (!curDirs.contains(w)) missing << w;
    for (const QString& w : wantFiles)
        if (!curFiles.contains(w)) missing << w;
    if (!missing.isEmpty()) m_fsw.addPaths(missing);
}

void SongsBinder::onDirEvent()
{
    if (m_dir.isEmpty()) return;
    addWatchPaths(); // new subdirs + defensive re-add after editor rewrites
    m_stability.stop();
    m_debounce.start(m_debounceMs);
}

void SongsBinder::onDebounce()
{
    m_lastScan = scanState();
    m_stabilityTries = 0;
    m_stability.start(m_stabilityMs);
}

void SongsBinder::onStabilityTick()
{
    const QStringList now = scanState();
    if (now == m_lastScan || ++m_stabilityTries >= kMaxStabilityTries) {
        emit folderChangedStable();
        return;
    }
    m_lastScan = now;
    m_stability.start(m_stabilityMs);
}

QStringList SongsBinder::scanState() const
{
    QStringList state;
    QDirIterator it(m_dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo& info = it.fileInfo();
        state << QStringLiteral("%1|%2|%3")
                     .arg(info.filePath())
                     .arg(info.size())
                     .arg(info.lastModified().toMSecsSinceEpoch());
    }
    state.sort();
    return state;
}

} // namespace ovc::watch
