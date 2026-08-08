#pragma once
#include <git/ops.h>
#include <git/registry.h>
#include <watch/songsbinder.h>
#include <watch/stablereader.h>
#include <QFutureWatcher>
#include <QObject>
#include <functional>
#include <optional>

namespace ovc::watch {

// The brain of v0.1: memory events in, snapshots out. Lives on the GUI/main
// thread; snapshot work runs on QtConcurrent, one job at a time. The app
// wires GameWatcher signals into the public slots; tests call them directly
// with fabricated state (no osu! needed).
class TrackingService : public QObject {
    Q_OBJECT
public:
    struct RestorePreflight {
        bool allowed = true;
        QString reason;
    };

    explicit TrackingService(QObject* parent = nullptr);

    SongsBinder& binder() { return m_binder; }
    // Editor clock source for the Ovc-Editor-Time trailer (app: GameWatcher).
    void setEditorTimeProvider(std::function<int()> provider);

    QList<ovc::git::MapsetEntry> tracked() const { return m_registry.entries; }
    QString activeRepoId() const { return m_activeRepoId; }
    const MemBeatmap& currentBeatmap() const { return m_current; }
    GameState gameState() const { return m_state; }

    std::optional<ovc::git::MapsetEntry> trackCurrentMapset(QString* err);
    void requestManualSnapshot(const QString& repoId);
    void setAutoSnapshot(const QString& repoId, bool on);
    RestorePreflight preflightRestore(const QString& repoId) const;
    std::optional<ovc::git::SnapshotResult> restore(const QString& repoId, const QByteArray& oid,
                                                    QString* err);

public slots:
    void onBeatmapChanged(const ovc::watch::MemBeatmap& map);
    void onBeatmapCleared();
    void onStateChanged(ovc::watch::GameState state);

signals:
    void activeMapsetChanged(const QString& repoId); // empty = untracked / none
    void snapshotTaken(const QString& repoId, const QString& subject, const QByteArray& oid);
    void snapshotFailed(const QString& repoId, const QString& reason);
    void trackedListChanged();

private:
    struct SnapJob {
        QString repoId;
        QString err;
        std::optional<ovc::git::SnapshotResult> res;
    };

    void onFolderChanged();
    void enqueueSnapshot(const QString& repoId, const QString& trigger);
    void runNextSnapshot();
    QString resolveIdentity(const MemBeatmap& map);
    QString activeDir() const;

    ovc::git::Registry m_registry;
    SongsBinder m_binder;
    QFutureWatcher<SnapJob> m_snapWatcher;
    std::function<int()> m_editorTime;
    MemBeatmap m_current;
    GameState m_state = GameState::Unknown;
    QString m_activeRepoId;
    QString m_activeDir;
    QHash<QString, QString> m_pending; // repoId -> trigger
    bool m_busy = false;
};

} // namespace ovc::watch
