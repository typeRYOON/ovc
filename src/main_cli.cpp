#include <git/gitcheck.h>
#include <git/ops.h>
#include <git/paths.h>
#include <git/registry.h>
#include <osu/canonical.h>
#include <osu/diff.h>
#include <osu/parser.h>
#include <osu/serializer.h>
#include <utils/timefmt.h>
#include <watch/gamewatcher.h>
#include <watch/trackingservice.h>
#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QTimer>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// Dev/test CLI. `probe` streams live memory-reader state; `gitcheck` proves
// the libgit2 link. More commands land with later milestones (parse, diff,
// track, snapshot, fswatch).

namespace {

QTextStream& out()
{
    static QTextStream ts(stdout);
    return ts;
}

const char* stateName(ovc::watch::GameState s)
{
    using ovc::watch::GameState;
    switch (s) {
    case GameState::Menu: return "Menu";
    case GameState::Edit: return "Edit";
    case GameState::Play: return "Play";
    case GameState::Exit: return "Exit";
    case GameState::SelectEdit: return "SelectEdit";
    case GameState::SelectPlay: return "SelectPlay";
    case GameState::ResultScreen: return "ResultScreen";
    case GameState::Lobby: return "Lobby";
    case GameState::SelectMulti: return "SelectMulti";
    default: return "Unknown";
    }
}

int runProbe(QCoreApplication& app)
{
    using namespace ovc::watch;
    out() << "probe: waiting for osu!.exe (Ctrl+C to quit)\n";
    out().flush();

    GameWatcher watcher;

    QObject::connect(&watcher, &GameWatcher::attachedChanged, [&](bool on) {
        out() << (on ? "[attach] connected, osuDir=" + watcher.osuDir() : "[attach] lost") << "\n";
        out().flush();
    });
    QObject::connect(&watcher, &GameWatcher::stateChanged, [&](GameState s) {
        out() << "[state] " << stateName(s) << "\n";
        out().flush();
    });
    QObject::connect(&watcher, &GameWatcher::beatmapChanged, [&](const MemBeatmap& m) {
        out() << "[map] " << m.artist << " - " << m.title << " [" << m.version << "]\n"
              << "      md5      " << m.md5 << "\n"
              << "      folder   " << m.folder << "\n"
              << "      osuPath  " << m.osuPath << "\n"
              << "      songsDir " << m.songsDir << "\n";
        out().flush();
    });
    QObject::connect(&watcher, &GameWatcher::beatmapCleared, [&]() {
        out() << "[map] cleared\n";
        out().flush();
    });
    QObject::connect(&watcher, &GameWatcher::attachBlocked, [&](const QString& hint) {
        out() << "[blocked] " << hint << "\n";
        out().flush();
    });

    // Editor clock, printed once per second while in Edit.
    QTimer clock;
    int lastShown = -1;
    QObject::connect(&clock, &QTimer::timeout, [&]() {
        if (watcher.gameState() != GameState::Edit) return;
        const int t = watcher.editorTimeMs();
        if (t >= 0 && t / 1000 != lastShown / 1000) {
            lastShown = t;
            out() << "[edit] t=" << t << " ms\n";
            out().flush();
        }
    });
    clock.start(250);

    return app.exec();
}

qsizetype firstDiff(const QByteArray& a, const QByteArray& b)
{
    const qsizetype n = qMin(a.size(), b.size());
    for (qsizetype i = 0; i < n; ++i)
        if (a.at(i) != b.at(i)) return i;
    return n; // sizes differ
}

int runParseCheck(const QStringList& args)
{
    const bool verbose = args.contains("-v");
    QStringList files;
    for (const QString& a : args) {
        if (a.startsWith('-')) continue;
        const QFileInfo info(a);
        if (info.isDir()) {
            QDirIterator it(a, {"*.osu"}, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) files << it.next();
        }
        else if (info.isFile()) {
            files << a;
        }
    }
    if (files.isEmpty()) {
        out() << "parse --check: no .osu files found\n";
        return 1;
    }

    QElapsedTimer timer;
    timer.start();
    int mismatches = 0, warned = 0;
    qint64 bytes = 0;
    QHash<int, int> versions;

    for (const QString& path : files) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            out() << "SKIP (unreadable) " << path << "\n";
            continue;
        }
        const QByteArray input = f.readAll();
        bytes += input.size();

        const auto res = ovc::osu::parseOsu(input);
        const QByteArray output = ovc::osu::serializeOsu(res.doc);
        ++versions[res.doc.formatVersion];

        if (output != input) {
            ++mismatches;
            out() << "MISMATCH " << path << "\n         first divergence at byte "
                  << firstDiff(input, output) << " (in " << input.size() << ", out "
                  << output.size() << ")\n";
        }
        if (!res.warnings.isEmpty()) {
            ++warned;
            if (verbose) {
                out() << "WARN     " << path << "\n";
                for (const auto& w : res.warnings)
                    out() << "         line " << w.lineNo << ": " << w.message << "\n";
            }
        }
    }

    out() << files.size() << " files, " << (bytes / 1024) << " KiB in " << timer.elapsed()
          << " ms — " << mismatches << " mismatches, " << warned << " with warnings\n";
    QList<int> vs = versions.keys();
    std::sort(vs.begin(), vs.end());
    out() << "format versions:";
    for (int v : vs) out() << " v" << v << "×" << versions[v];
    out() << "\n";
    out().flush();
    return mismatches == 0 ? 0 : 1;
}

void renderTimingRow(const ovc::osu::TimingPoint& tp)
{
    using ovc::utils::msToClock;
    out() << msToClock(static_cast<qint64>(tp.timeMs)) << "  ";
    if (tp.uninherited)
        out() << QString::number(tp.bpm(), 'g', 6) << " BPM";
    else
        out() << "SV " << QString::number(tp.sv(), 'g', 4) << "×";
    if (tp.kiai()) out() << "  kiai";
}

int runDiff(const QStringList& args)
{
    using namespace ovc::osu;
    using ovc::utils::msToClock;
    if (args.size() < 2) {
        out() << "usage: ovc-cli diff <a.osu> <b.osu>\n";
        return 1;
    }
    CanonicalMap maps[2];
    for (int i = 0; i < 2; ++i) {
        QFile f(args.at(i));
        if (!f.open(QIODevice::ReadOnly)) {
            out() << "cannot read " << args.at(i) << "\n";
            return 1;
        }
        const auto parsed = parseOsu(f.readAll());
        QList<ParseWarning> warnings = parsed.warnings;
        maps[i] = canonicalize(parsed.doc, &warnings);
        for (const auto& w : warnings)
            out() << args.at(i) << ": warning: " << w.message << "\n";
    }

    const BeatmapDiff d = diffBeatmaps(maps[0], maps[1]);
    if (d.isEmpty()) {
        out() << "no semantic changes\n";
        out().flush();
        return 0;
    }

    static const QHash<int, QByteArray> kSec = {
        {int(SectionId::General), "[General]"},
        {int(SectionId::Editor), "[Editor]"},
        {int(SectionId::Metadata), "[Metadata]"},
        {int(SectionId::Difficulty), "[Difficulty]"},
    };
    for (const KvDiff& sec : d.kv) {
        out() << kSec.value(int(sec.section), "[?]") << "\n";
        for (const FieldChange& f : sec.changes) {
            if (f.before.isEmpty()) out() << "  + " << f.key << ": " << f.after.raw << "\n";
            else if (f.after.isEmpty()) out() << "  − " << f.key << ": " << f.before.raw << "\n";
            else out() << "  ~ " << f.key << ": " << f.before.raw << " → " << f.after.raw << "\n";
        }
    }

    if (d.modeChanged) out() << "mode changed — hitobject diff skipped\n";
    if (d.keyCountChanged)
        out() << d.keyCountBefore << "K → " << d.keyCountAfter
              << "K — every column remaps, hitobject diff skipped\n";

    if (!d.events.isEmpty()) {
        out() << "Events\n";
        if (d.events.background)
            out() << "  ~ background " << d.events.background->before.raw << " → "
                  << d.events.background->after.raw << "\n";
        if (d.events.video)
            out() << "  ~ video " << d.events.video->before.raw << " → "
                  << d.events.video->after.raw << "\n";
        for (const BreakChange& b : d.events.breaks) {
            if (b.op == ChangeOp::Added)
                out() << "  + break " << msToClock(b.after.startMs) << "–"
                      << msToClock(b.after.endMs) << "\n";
            else if (b.op == ChangeOp::Removed)
                out() << "  − break " << msToClock(b.before.startMs) << "–"
                      << msToClock(b.before.endMs) << "\n";
            else
                out() << "  ~ break " << msToClock(b.before.startMs) << " end "
                      << msToClock(b.before.endMs) << " → " << msToClock(b.after.endMs)
                      << "\n";
        }
        if (d.events.storyboardChanged)
            out() << "  storyboard lines +" << d.events.sbLinesAdded << " −"
                  << d.events.sbLinesRemoved << "\n";
    }

    if (!d.timing.isEmpty()) {
        out() << "Timing\n";
        for (const TimingChange& t : d.timing) {
            if (t.op == ChangeOp::Added) {
                out() << "  + ";
                renderTimingRow(t.after);
            }
            else if (t.op == ChangeOp::Removed) {
                out() << "  − ";
                renderTimingRow(t.before);
            }
            else {
                out() << "  ~ " << msToClock(t.timeQ / 1000) << " ";
                QStringList bits;
                for (const FieldChange& f : t.fields)
                    bits << QStringLiteral("%1 %2 → %3")
                                .arg(QString::fromUtf8(f.key),
                                     QString::fromUtf8(f.before.raw),
                                     QString::fromUtf8(f.after.raw));
                out() << bits.join(", ");
            }
            out() << "\n";
        }
    }

    int shown = 0;
    int visible = 0;
    for (const NoteChange& n : d.notes)
        if (!n.moveSuppressed) ++visible;
    if (visible > 0) {
        out() << "Notes\n";
        for (const NoteChange& n : d.notes) {
            if (n.moveSuppressed) continue;
            if (shown++ == 200) {
                out() << "  … and " << (visible - 200) << " more\n";
                break;
            }
            if (n.movedFromColumn >= 0) {
                out() << "  → " << msToClock(n.timeMs) << " col " << n.movedFromColumn
                      << "→" << n.column << " (moved)\n";
                continue;
            }
            const char* mark = n.op == ChangeOp::Added ? "+" : n.op == ChangeOp::Removed ? "−" : "~";
            out() << "  " << mark << " " << msToClock(n.timeMs) << " col " << n.column;
            const CanonicalNote& show = n.op == ChangeOp::Removed ? n.before : n.after;
            if (show.isHold) out() << " hold →" << msToClock(show.endTimeMs);
            for (const FieldChange& f : n.fields)
                out() << "  " << f.key << " " << f.before.raw << " → " << f.after.raw;
            out() << "\n";
        }
    }

    auto renderList = [](const char* name, const ovc::osu::ListDiff& l) {
        if (l.isEmpty()) return;
        out() << name << " ";
        QStringList bits;
        for (const QByteArray& v : l.added) bits << "+" + QString::fromUtf8(v);
        for (const QByteArray& v : l.removed) bits << "−" + QString::fromUtf8(v);
        out() << bits.join(' ') << "\n";
    };
    renderList("Bookmarks", d.bookmarks);
    renderList("Tags", d.tags);

    out() << "— " << d.summary();
    const auto range = d.affectedTimeRange();
    if (range.first >= 0)
        out() << "  (" << msToClock(range.first) << " – " << msToClock(range.second) << ")";
    out() << "\n";
    out().flush();
    return 2;
}

// Save-behavior spike: raw ReadDirectoryChangesW event log with timings, to
// see exactly what osu!'s editor does on save (rewrite? temp+rename? bursts?).
int runFswatch(const QStringList& args)
{
#ifndef Q_OS_WIN
    out() << "fswatch is Windows-only\n";
    return 1;
#else
    if (args.isEmpty()) {
        out() << "usage: ovc-cli fswatch <folder>\n";
        return 1;
    }
    const QString dir = QDir::toNativeSeparators(args.first());
    HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(dir.utf16()), FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        out() << "cannot open " << dir << "\n";
        return 1;
    }
    out() << "watching " << dir << " (Ctrl+C to quit)\n";
    out().flush();

    QElapsedTimer clock;
    clock.start();
    qint64 lastMs = -1;
    alignas(DWORD) char buf[64 * 1024];
    while (true) {
        DWORD bytes = 0;
        if (!ReadDirectoryChangesW(h, buf, sizeof(buf), TRUE,
                                   FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                       FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                       FILE_NOTIFY_CHANGE_CREATION,
                                   &bytes, nullptr, nullptr)) {
            out() << "ReadDirectoryChangesW failed\n";
            return 1;
        }
        const qint64 now = clock.elapsed();
        const char* p = buf;
        while (true) {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
            const char* action = "?";
            switch (info->Action) {
            case FILE_ACTION_ADDED: action = "ADDED   "; break;
            case FILE_ACTION_REMOVED: action = "REMOVED "; break;
            case FILE_ACTION_MODIFIED: action = "MODIFIED"; break;
            case FILE_ACTION_RENAMED_OLD_NAME: action = "REN_OLD "; break;
            case FILE_ACTION_RENAMED_NEW_NAME: action = "REN_NEW "; break;
            }
            const QString name = QString::fromWCharArray(
                info->FileName, static_cast<int>(info->FileNameLength / sizeof(wchar_t)));
            out() << QString::asprintf("%8lld ms  %s+%-5lld  %s  ", now,
                                       lastMs < 0 ? "" : "\xce\x94", lastMs < 0 ? 0 : now - lastMs,
                                       action)
                  << name << "\n";
            if (info->NextEntryOffset == 0) break;
            p += info->NextEntryOffset;
        }
        lastMs = now;
        out().flush();
    }
#endif
}

int runWatch(QCoreApplication& app)
{
    using namespace ovc::watch;
    out() << "watch: full tracking loop (Ctrl+C to quit)\n";
    out().flush();

    GameWatcher watcher;
    TrackingService svc;
    QObject::connect(&watcher, &GameWatcher::beatmapChanged, &svc,
                     &TrackingService::onBeatmapChanged);
    QObject::connect(&watcher, &GameWatcher::beatmapCleared, &svc,
                     &TrackingService::onBeatmapCleared);
    QObject::connect(&watcher, &GameWatcher::stateChanged, &svc,
                     &TrackingService::onStateChanged);
    svc.setEditorTimeProvider([&watcher]() { return watcher.editorTimeMs(); });

    QObject::connect(&watcher, &GameWatcher::attachedChanged, [](bool on) {
        out() << (on ? "[attach] connected" : "[attach] lost") << "\n";
        out().flush();
    });
    QObject::connect(&svc, &TrackingService::activeMapsetChanged, [&svc](const QString& rid) {
        if (rid.isEmpty()) {
            const auto& m = svc.currentBeatmap();
            if (!m.folder.isEmpty())
                out() << "[map] untracked: " << m.folder << "  (track with: ovc-cli track \""
                      << m.songsDir << "/" << m.folder << "\")\n";
        }
        else {
            out() << "[map] tracking " << rid << "\n";
        }
        out().flush();
    });
    QObject::connect(&svc, &TrackingService::snapshotTaken,
                     [](const QString&, const QString& subject, const QByteArray& oid) {
                         out() << "[snap] " << oid.left(7) << "  " << subject << "\n";
                         out().flush();
                     });
    QObject::connect(&svc, &TrackingService::snapshotFailed,
                     [](const QString&, const QString& reason) {
                         out() << "[snap] FAILED: " << reason << "\n";
                         out().flush();
                     });
    return app.exec();
}

int runRestore(const QStringList& args)
{
    using namespace ovc::watch;
    if (args.size() < 2) {
        out() << "usage: ovc-cli restore <repoId|folder> <commit> [--force]\n";
        return 1;
    }
    const bool force = args.contains("--force");
    ovc::git::Registry reg = ovc::git::Registry::load();
    ovc::git::MapsetEntry* entry = reg.findByRepoId(args.at(0));
    if (!entry) entry = reg.findBySongsPath(args.at(0));
    if (!entry) {
        out() << "not tracked: " << args.at(0) << "\n";
        return 1;
    }

    // Refuse while osu! is editing this very mapset (it would overwrite the
    // restored files on its next save).
    if (!force) {
        const auto pids = ProcessHandle::findProcesses({QStringLiteral("osu!.exe")});
        if (!pids.empty()) {
            ProcessHandle proc;
            if (proc.open(pids.front()) && !proc.is64bit()) {
                StableReader reader(proc);
                out() << "checking osu! state…\n";
                out().flush();
                if (reader.resolve() && reader.status() == GameState::Edit) {
                    MemBeatmap m;
                    if (reader.readBeatmap(m) &&
                        QDir::cleanPath(m.songsDir + "/" + m.folder)
                                .compare(QDir::cleanPath(entry->songsPath),
                                         Qt::CaseInsensitive) == 0) {
                        out() << "refused: osu! is editing this mapset right now. Close the "
                                 "editor (or pass --force).\n";
                        return 1;
                    }
                }
            }
        }
    }

    QString err;
    const auto res = ovc::git::restoreMapset(*entry, args.at(1).toUtf8(), &err);
    if (!res) {
        if (!err.isEmpty()) {
            out() << "restore failed: " << err << "\n";
            return 1;
        }
        out() << "already at that state\n";
        return 0;
    }
    out() << res->commitOid.left(7) << "  " << res->subject << "\n"
          << "Press F5 in song select before reopening the map in osu!.\n";
    out().flush();
    return 0;
}

ovc::git::MapsetEntry* resolveEntry(ovc::git::Registry& reg, const QString& arg)
{
    if (auto* e = reg.findByRepoId(arg)) return e;
    return reg.findBySongsPath(arg);
}

int runTrack(const QStringList& args)
{
    if (args.isEmpty()) {
        out() << "usage: ovc-cli track <mapset folder>\n";
        return 1;
    }
    QString err;
    const auto entry = ovc::git::trackMapset(args.first(), &err);
    if (!entry) {
        out() << "track failed: " << err << "\n";
        return 1;
    }
    out() << "tracked " << entry->artist << " - " << entry->title << " (" << entry->creator
          << ")\n  repoId " << entry->repoId << "\n  repo   " << entry->repoDir() << "\n";
    out().flush();
    return 0;
}

int runSnapshot(const QStringList& args)
{
    if (args.isEmpty()) {
        out() << "usage: ovc-cli snapshot <repoId|mapset folder>\n";
        return 1;
    }
    ovc::git::Registry reg = ovc::git::Registry::load();
    const auto* entry = resolveEntry(reg, args.first());
    if (!entry) {
        out() << "not tracked: " << args.first() << "\n";
        return 1;
    }
    QString err;
    const auto res = ovc::git::snapshotMapset(*entry, QStringLiteral("manual"), {}, &err);
    if (!res) {
        if (!err.isEmpty()) {
            out() << "snapshot failed: " << err << "\n";
            return 1;
        }
        out() << "no changes\n";
        return 0;
    }
    out() << res->commitOid.left(7) << "  " << res->subject << "\n";
    out().flush();
    return 0;
}

int runLog(const QStringList& args)
{
    if (args.isEmpty()) {
        out() << "usage: ovc-cli log <repoId|mapset folder>\n";
        return 1;
    }
    ovc::git::Registry reg = ovc::git::Registry::load();
    const auto* entry = resolveEntry(reg, args.first());
    if (!entry) {
        out() << "not tracked: " << args.first() << "\n";
        return 1;
    }
    auto repo = ovc::git::ShadowRepo::open(entry->repoDir());
    if (!repo) {
        out() << "repo missing: " << entry->repoDir() << "\n";
        return 1;
    }
    const auto commits = repo->log(200);
    for (const auto& c : commits) {
        out() << c.oid.left(7) << "  " << c.when.toLocalTime().toString("yyyy-MM-dd HH:mm:ss")
              << "  " << c.subject << "\n";
    }
    out() << commits.size() << " snapshots\n";
    out().flush();
    return 0;
}

int runGitCheck()
{
    using namespace ovc::git;
    out() << "libgit2 " << libgit2Version() << "\n";
    QString err;
    const bool ok = selfTest(&err);
    out() << "selftest " << (ok ? "ok" : "FAILED: " + err) << "\n";
    out().flush();
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("ovc");
    ovc::git::LibGit libgit;

    const QStringList args = app.arguments();
    const QString cmd = args.size() > 1 ? args.at(1) : QString();

    if (cmd == "probe") return runProbe(app);
    if (cmd == "gitcheck") return runGitCheck();
    if (cmd == "parse" && args.size() > 2 && args.at(2) == "--check")
        return runParseCheck(args.mid(3));
    if (cmd == "diff") return runDiff(args.mid(2));
    if (cmd == "track") return runTrack(args.mid(2));
    if (cmd == "snapshot") return runSnapshot(args.mid(2));
    if (cmd == "log") return runLog(args.mid(2));
    if (cmd == "fswatch") return runFswatch(args.mid(2));
    if (cmd == "watch") return runWatch(app);
    if (cmd == "restore") return runRestore(args.mid(2));

    out() << "usage: ovc-cli <command>\n"
             "  probe                     stream osu! memory-reader state\n"
             "  watch                     full tracking loop: auto-snapshot the active mapset\n"
             "  gitcheck                  print libgit2 version and run a scratch-repo self test\n"
             "  parse --check <path> [-v] verify lossless round-trip of .osu file(s) or dir\n"
             "  diff <a.osu> <b.osu>      semantic diff of two difficulties\n"
             "  track <folder>            start tracking a mapset folder (shadow repo + import)\n"
             "  snapshot <id|folder>      mirror + commit current state if changed\n"
             "  log <id|folder>           list snapshots of a tracked mapset\n"
             "  restore <id|folder> <oid> write an old snapshot back into the Songs folder\n"
             "  fswatch <folder>          log raw filesystem events (save-behavior spike)\n";
    out().flush();
    return cmd.isEmpty() ? 0 : 1;
}
