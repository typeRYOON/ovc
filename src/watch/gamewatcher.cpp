#include <watch/gamewatcher.h>
#include <QCoreApplication>
#include <QFileInfo>
#include <QtConcurrent>

namespace ovc::watch {

namespace {
constexpr int kTickMs = 800;
constexpr int kAttachEveryTicks = 4; // process discovery every ~3.2s while down
} // namespace

GameWatcher::GameWatcher(QObject* parent) : QObject(parent)
{
    connect(&m_timer, &QTimer::timeout, this, &GameWatcher::tick);
    connect(&m_scanWatcher, &QFutureWatcher<bool>::finished, this,
            [this]() { applyScan(m_scanWatcher.result()); });
    m_timer.start(kTickMs);
    tick();
}

int GameWatcher::editorTimeMs() const
{
    if (!m_attached || !m_reader) return -1;
    return m_reader->editorTimeMs();
}

void GameWatcher::detach()
{
    if (m_attached) {
        m_attached = false;
        m_beatmap = MemBeatmap{};
        m_state = GameState::Unknown;
        emit beatmapCleared();
        emit attachedChanged(false);
    }
    m_proc.reset();
    m_reader.reset();
}

void GameWatcher::tryAttach()
{
    const auto pids = ProcessHandle::findProcesses({QStringLiteral("osu!.exe")});
    if (pids.empty()) return;

    auto proc = std::make_unique<ProcessHandle>();
    if (!proc->open(pids.front()) || proc->is64bit()) return;

    m_proc = std::move(proc);
    m_reader = std::make_unique<StableReader>(*m_proc);
    m_osuDir = QFileInfo(windowsPathToHost(m_proc->imagePath(), m_proc->winePrefix()))
                   .absolutePath();

    m_scanning = true;
    StableReader* reader = m_reader.get();
    m_scanWatcher.setFuture(QtConcurrent::run([reader]() { return reader->resolve(); }));
}

void GameWatcher::applyScan(bool ok)
{
    m_scanning = false;
    if (!ok || !m_reader) {
        // "Found nothing" retries quietly; "not allowed to look" never fixes
        // itself — tell the user once per streak instead of spinning. A live
        // osu! always has hundreds of readable regions, so an empty region
        // list means /proc/<pid>/maps itself was denied (non-dumpable target)
        // — same permission wall as an EPERM read.
        const bool blocked =
            m_proc && (m_proc->readDenied() || m_proc->queryRegions().empty());
        if (blocked && !m_blockNotified) {
            m_blockNotified = true;
            emit attachBlocked(
                tr("osu! found, but reading its memory was denied (ptrace). Grant it once:  "
                   "sudo setcap cap_sys_ptrace=eip \"%1\"  — then restart ovc. "
                   "(umu/bwrap launchers make osu! non-dumpable, so "
                   "kernel.yama.ptrace_scope=0 alone is not enough there.)")
                    .arg(QCoreApplication::applicationFilePath()));
        }
        m_proc.reset();
        m_reader.reset();
        return;
    }
    m_attached = true;
    m_blockNotified = false;
    emit attachedChanged(true);
}

void GameWatcher::tick()
{
    if (m_scanning) return;

    if (!m_attached) {
        if (--m_attachCooldown <= 0) {
            m_attachCooldown = kAttachEveryTicks;
            tryAttach();
        }
        return;
    }

    if (!m_proc->isAlive()) {
        detach();
        return;
    }

    const GameState st = m_reader->status();
    if (st != m_state) {
        m_state = st;
        emit stateChanged(st);
    }

    const QString md5 = m_reader->currentMd5();
    if (md5.isEmpty()) {
        if (!m_beatmap.md5.isEmpty()) {
            m_beatmap = MemBeatmap{};
            emit beatmapCleared();
        }
        return;
    }
    if (md5 != m_beatmap.md5) {
        MemBeatmap map;
        if (m_reader->readBeatmap(map) && !map.md5.isEmpty()) {
            m_beatmap = map;
            emit beatmapChanged(map);
        }
    }
}

} // namespace ovc::watch
