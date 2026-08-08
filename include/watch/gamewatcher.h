#pragma once
#include <watch/stablereader.h>
#include <QFutureWatcher>
#include <QObject>
#include <QTimer>
#include <memory>

namespace ovc::watch {

// Polls osu! stable at low cost: attach + one AOB scan per game launch (off
// the UI thread), then a couple of small reads per tick. Emits only on change.
class GameWatcher : public QObject {
    Q_OBJECT
public:
    explicit GameWatcher(QObject* parent = nullptr);

    bool attached() const { return m_attached; }
    GameState gameState() const { return m_state; }
    const MemBeatmap& beatmap() const { return m_beatmap; }
    bool hasBeatmap() const { return !m_beatmap.md5.isEmpty(); }
    QString osuDir() const { return m_osuDir; }
    // On-demand audio-clock read (editor position while in Edit); -1 when
    // detached. Stateless ReadProcessMemory, safe from any thread.
    int editorTimeMs() const;

signals:
    void attachedChanged(bool attached);
    void stateChanged(ovc::watch::GameState state);
    void beatmapChanged(const ovc::watch::MemBeatmap& map);
    void beatmapCleared();
    // osu! was found but memory reads came back EPERM — the scan can never
    // succeed until the user grants ptrace rights. Emitted once per streak.
    void attachBlocked(const QString& hint);

private:
    void tick();
    void tryAttach();
    void applyScan(bool ok);
    void detach();

    std::unique_ptr<ProcessHandle> m_proc;
    std::unique_ptr<StableReader> m_reader;
    QTimer m_timer;
    QFutureWatcher<bool> m_scanWatcher;
    bool m_scanning = false;
    bool m_attached = false;
    bool m_blockNotified = false;
    int m_attachCooldown = 0;
    GameState m_state = GameState::Unknown;
    MemBeatmap m_beatmap;
    QString m_osuDir;
};

} // namespace ovc::watch
