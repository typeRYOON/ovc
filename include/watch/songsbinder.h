#pragma once
#include <QFileSystemWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>

namespace ovc::watch {

// Watches ONE mapset folder (the active one — never all of Songs; thousands
// of subdirs would exhaust watcher handles). Emits folderChangedStable once a
// save burst has settled: debounce after the last FS event, then a stability
// probe that re-stats the folder until two scans agree.
class SongsBinder : public QObject {
    Q_OBJECT
public:
    explicit SongsBinder(QObject* parent = nullptr);

    void setTimings(int debounceMs, int stabilityMs); // tests shrink these
    void bind(const QString& mapsetDir);              // also watches immediate subdirs (sb/, hs/)
    void unbind();
    QString boundDir() const { return m_dir; }
    // Emits folderChangedStable immediately if a change was still pending
    // (map switch mid-debounce must not lose the last save). True if emitted.
    bool flushPending();

signals:
    void folderChangedStable();

private:
    void onDirEvent();
    void onDebounce();
    void onStabilityTick();
    void addWatchPaths();
    QStringList scanState() const;

    QFileSystemWatcher m_fsw;
    QTimer m_debounce, m_stability;
    QString m_dir;
    QStringList m_lastScan;
    int m_debounceMs = 800;
    int m_stabilityMs = 150;
    int m_stabilityTries = 0;
};

} // namespace ovc::watch
