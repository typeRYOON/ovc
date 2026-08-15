#include <git/gitcheck.h>
#include <git/ops.h>
#include <git/paths.h>
#include <git/registry.h>
#include <ovccore/canonical.h>
#include <ovccore/diff.h>
#include <ovccore/json.h>
#include <ovccore/merge.h>
#include <ovccore/parser.h>
#include <ovccore/timefmt.h>
#include <git/bundle.h>
#include <git/mergesessions.h>
#include <serve/localserver.h>
#include <utils/config.h>
#include <watch/gamewatcher.h>
#include <watch/trackingservice.h>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QTimer>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
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

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

std::string_view sv(const QByteArray& b)
{
    return {b.constData(), size_t(b.size())};
}

size_t firstDiff(std::string_view a, std::string_view b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return i;
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

        const auto res = ovc::core::parseOsu(sv(input));
        const std::string output = ovc::core::serializeOsu(res.doc);
        ++versions[res.doc.formatVersion];

        if (output != sv(input)) {
            ++mismatches;
            out() << "MISMATCH " << path << "\n         first divergence at byte "
                  << qint64(firstDiff(sv(input), output)) << " (in " << input.size()
                  << ", out " << qint64(output.size()) << ")\n";
        }
        if (!res.warnings.empty()) {
            ++warned;
            if (verbose) {
                out() << "WARN     " << path << "\n";
                for (const auto& w : res.warnings)
                    out() << "         line " << w.lineNo << ": " << qs(w.message) << "\n";
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

void renderTimingRow(const ovc::core::TimingPoint& tp)
{
    out() << qs(ovc::core::msToClock(qint64(tp.timeMs))) << "  ";
    if (tp.uninherited)
        out() << QString::number(tp.bpm(), 'g', 6) << " BPM";
    else
        out() << "SV " << QString::number(tp.sv(), 'g', 4) << "x";
    if (tp.kiai()) out() << "  kiai";
}

int runDiff(const QStringList& args)
{
    using namespace ovc::core;
    if (args.size() < 2) {
        out() << "usage: ovc-cli diff <a.osu> <b.osu> [--json]\n";
        return 1;
    }
    const bool json = args.contains("--json");
    CanonicalMap maps[2];
    for (int i = 0; i < 2; ++i) {
        QFile f(args.at(i));
        if (!f.open(QIODevice::ReadOnly)) {
            out() << "cannot read " << args.at(i) << "\n";
            return 1;
        }
        const QByteArray bytes = f.readAll();
        const auto parsed = parseOsu(sv(bytes));
        std::vector<ParseWarning> warnings = parsed.warnings;
        maps[i] = canonicalize(parsed.doc, &warnings);
        if (!json)
            for (const auto& w : warnings)
                out() << args.at(i) << ": warning: " << qs(w.message) << "\n";
    }

    const BeatmapDiff d = diffBeatmaps(maps[0], maps[1]);
    if (json) {
        out() << qs(diffToJson(d)) << "\n";
        out().flush();
        return d.empty() ? 0 : 2;
    }
    if (d.empty()) {
        out() << "no semantic changes\n";
        out().flush();
        return 0;
    }

    for (const KvDiff& sec : d.kv) {
        out() << "[" << sectionName(sec.section) << "]\n";
        for (const FieldChange& f : sec.changes) {
            if (f.before.empty()) out() << "  + " << qs(f.key) << ": " << qs(f.after.raw) << "\n";
            else if (f.after.empty())
                out() << "  - " << qs(f.key) << ": " << qs(f.before.raw) << "\n";
            else
                out() << "  ~ " << qs(f.key) << ": " << qs(f.before.raw) << " -> "
                      << qs(f.after.raw) << "\n";
        }
    }

    if (d.modeChanged) out() << "mode changed - hitobject diff skipped\n";
    if (d.keyCountChanged)
        out() << d.keyCountBefore << "K -> " << d.keyCountAfter
              << "K - every column remaps, hitobject diff skipped\n";

    if (!d.events.empty()) {
        out() << "Events\n";
        if (d.events.background)
            out() << "  ~ background " << qs(d.events.background->before.raw) << " -> "
                  << qs(d.events.background->after.raw) << "\n";
        for (const BreakChange& b : d.events.breaks) {
            if (b.op == ChangeOp::Added)
                out() << "  + break " << qs(msToClock(b.after.startMs)) << "-"
                      << qs(msToClock(b.after.endMs)) << "\n";
            else if (b.op == ChangeOp::Removed)
                out() << "  - break " << qs(msToClock(b.before.startMs)) << "-"
                      << qs(msToClock(b.before.endMs)) << "\n";
            else
                out() << "  ~ break " << qs(msToClock(b.before.startMs)) << " end "
                      << qs(msToClock(b.before.endMs)) << " -> " << qs(msToClock(b.after.endMs))
                      << "\n";
        }
        if (d.events.storyboardChanged)
            out() << "  storyboard lines +" << d.events.sbLinesAdded << " -"
                  << d.events.sbLinesRemoved << "\n";
    }

    if (!d.timing.empty()) {
        out() << "Timing\n";
        for (const TimingChange& t : d.timing) {
            if (t.op == ChangeOp::Added) {
                out() << "  + ";
                renderTimingRow(t.after);
            }
            else if (t.op == ChangeOp::Removed) {
                out() << "  - ";
                renderTimingRow(t.before);
            }
            else {
                out() << "  ~ " << qs(msToClock(t.timeQ / 1000)) << " ";
                QStringList bits;
                for (const FieldChange& f : t.fields)
                    bits << QStringLiteral("%1 %2 -> %3").arg(qs(f.key), qs(f.before.raw),
                                                              qs(f.after.raw));
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
                out() << "  ... and " << (visible - 200) << " more\n";
                break;
            }
            if (n.movedFromColumn >= 0) {
                out() << "  > " << qs(msToClock(n.timeMs)) << " col " << n.movedFromColumn
                      << "->" << n.column << " (moved)\n";
                continue;
            }
            const char* mark = n.op == ChangeOp::Added     ? "+"
                               : n.op == ChangeOp::Removed ? "-"
                                                           : "~";
            out() << "  " << mark << " " << qs(msToClock(n.timeMs)) << " col " << n.column;
            const CanonicalNote& show = n.op == ChangeOp::Removed ? n.before : n.after;
            if (show.isHold) out() << " hold >" << qs(msToClock(show.endTimeMs));
            for (const FieldChange& f : n.fields)
                out() << "  " << qs(f.key) << " " << qs(f.before.raw) << " -> "
                      << qs(f.after.raw);
            out() << "\n";
        }
    }

    auto renderList = [](const char* name, const ovc::core::ListDiff& l) {
        if (l.empty()) return;
        out() << name << " ";
        QStringList bits;
        for (const std::string& v : l.added) bits << "+" + qs(v);
        for (const std::string& v : l.removed) bits << "-" + qs(v);
        out() << bits.join(' ') << "\n";
    };
    renderList("Bookmarks", d.bookmarks);
    renderList("Tags", d.tags);

    out() << "-- " << qs(d.summary());
    const auto range = d.affectedTimeRange();
    if (range.first >= 0)
        out() << "  (" << qs(msToClock(range.first)) << " - " << qs(msToClock(range.second))
              << ")";
    out() << "\n";
    out().flush();
    return 2;
}

// Save-behavior spike: raw ReadDirectoryChangesW event log with timings.
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
                info->FileName, int(info->FileNameLength / sizeof(wchar_t)));
            out() << QString::asprintf("%8lld ms  d+%-6lld  %s  ", now,
                                       lastMs < 0 ? 0 : now - lastMs, action)
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

ovc::git::MapsetEntry* resolveEntry(ovc::git::Registry& reg, const QString& arg)
{
    if (auto* e = reg.findByRepoId(arg)) return e;
    return reg.findBySongsPath(arg);
}

// Looser match for untrack/list: repoId/path first, then an exact folder or title
// (case-insensitive), then a UNIQUE case-insensitive substring of the folder or
// "Artist - Title" (ambiguous -> null, so a vague arg can't nuke the wrong set).
ovc::git::MapsetEntry* resolveEntryLoose(ovc::git::Registry& reg, const QString& arg)
{
    if (auto* e = resolveEntry(reg, arg)) return e;
    for (auto& e : reg.entries)
        if (e.folderName.compare(arg, Qt::CaseInsensitive) == 0
            || e.title.compare(arg, Qt::CaseInsensitive) == 0)
            return &e;
    ovc::git::MapsetEntry* hit = nullptr;
    int n = 0;
    for (auto& e : reg.entries) {
        const QString label = e.artist + QStringLiteral(" - ") + e.title;
        if (e.folderName.contains(arg, Qt::CaseInsensitive)
            || label.contains(arg, Qt::CaseInsensitive)) {
            hit = &e;
            ++n;
        }
    }
    return n == 1 ? hit : nullptr;
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
    ovc::git::MapsetEntry* entry = resolveEntry(reg, args.at(0));
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
                out() << "checking osu! state...\n";
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

int runList(const QStringList&)
{
    ovc::git::Registry reg = ovc::git::Registry::load();
    if (reg.entries.isEmpty()) {
        out() << "no tracked mapsets\n";
        out().flush();
        return 0;
    }
    for (const auto& e : reg.entries)
        out() << e.repoId << "  " << e.artist << " - " << e.title
              << (e.autoSnapshot ? "" : "  [auto-snapshot off]") << "\n"
              << "    " << e.folderName << "\n";
    out().flush();
    return 0;
}

int runUntrack(const QStringList& args)
{
    QString name;
    for (const QString& a : args)
        if (!a.startsWith(QStringLiteral("--"))) {
            name = a;
            break;
        }
    if (name.isEmpty()) {
        out() << "usage: ovc-cli untrack <repoId|folder|title> [--keep-data]\n"
                 "  removes the mapset from ovc; deletes its tracked history unless --keep-data.\n"
                 "  your osu! Songs folder is never touched.\n";
        return 1;
    }
    const bool keep = args.contains(QStringLiteral("--keep-data"));
    ovc::git::Registry reg = ovc::git::Registry::load();
    ovc::git::MapsetEntry* e = resolveEntryLoose(reg, name);
    if (!e) {
        out() << "not tracked (or ambiguous): " << name << "\n";
        return 1;
    }
    const QString rid = e->repoId;
    const QString label = e->artist + QStringLiteral(" - ") + e->title;
    const QString dir = e->repoDir();
    reg.removeByRepoId(rid);
    QString err;
    if (!reg.save(&err)) {
        out() << "untrack failed: " << err << "\n";
        return 1;
    }
    if (!keep) QDir(dir).removeRecursively();
    out() << "untracked " << label << "  (" << rid << ")\n"
          << (keep ? "  kept history at " : "  deleted ") << dir << "\n"
          << "  note: if the ovc app is running, quit + reopen it so it doesn't re-add this.\n";
    out().flush();
    return 0;
}

int runRelink(const QStringList& args)
{
    if (args.size() < 2) {
        out() << "usage: ovc-cli relink <repoId|folder|title> <newFolder>\n"
                 "  re-point a tracked set at a renamed/moved folder (e.g. after an upload\n"
                 "  assigned it a set ID). Refreshes the stored IDs from the new .osu files.\n";
        return 1;
    }
    ovc::git::Registry reg = ovc::git::Registry::load();
    ovc::git::MapsetEntry* e = resolveEntryLoose(reg, args.at(0));
    if (!e) {
        out() << "not tracked (or ambiguous): " << args.at(0) << "\n";
        return 1;
    }
    QString err;
    if (!ovc::git::relinkEntry(*e, args.at(1), &err) || !reg.save(&err)) {
        out() << "relink failed: " << err << "\n";
        return 1;
    }
    out() << "relinked " << e->artist << " - " << e->title << "  (" << e->repoId << ")\n"
          << "  now at " << e->songsPath << "\n"
          << "  setId  " << e->beatmapSetId << "\n"
          << "  note: if the ovc app is running, quit + reopen it so it uses the new path.\n";
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

// git merge-driver shape: `merge-file %O %A %B` — %A (ours) is overwritten
// with the merged result; exit 0 clean, 1 on conflict (ours wins each, so the
// file is still usable). Register with:
//   git config merge.osu.driver "ovc-cli merge-file %O %A %B"
int runMergeFile(const QStringList& args)
{
    using namespace ovc::core;
    if (args.size() < 3) {
        out() << "usage: ovc-cli merge-file <base> <ours> <theirs>\n";
        return 2;
    }
    auto read = [](const QString& path, bool* ok) {
        QFile f(path);
        *ok = f.open(QIODevice::ReadOnly);
        return *ok ? f.readAll() : QByteArray();
    };
    bool okB = false, okO = false, okT = false;
    const QByteArray baseBytes = read(args.at(0), &okB);
    const QByteArray oursBytes = read(args.at(1), &okO);
    const QByteArray theirsBytes = read(args.at(2), &okT);
    if (!okO || !okT) {
        out() << "merge-file: cannot read ours/theirs\n";
        return 2;
    }
    const CanonicalMap base = canonicalize(parseOsu(sv(baseBytes)).doc);
    const CanonicalMap ours = canonicalize(parseOsu(sv(oursBytes)).doc);
    const CanonicalMap theirs = canonicalize(parseOsu(sv(theirsBytes)).doc);

    const MergeResult r = merge3(base, ours, theirs);
    if (r.wholeFileConflict) {
        out() << "merge-file: cannot auto-merge — " << qs(r.reason)
              << "\n(left ours untouched; resolve by hand)\n";
        return 1; // leave ours as-is for a manual resolution
    }

    const std::string merged = emitCanonical(r.merged);
    QFile ours_out(args.at(1));
    if (!ours_out.open(QIODevice::WriteOnly)) {
        out() << "merge-file: cannot write " << args.at(1) << "\n";
        return 2;
    }
    ours_out.write(merged.data(), qint64(merged.size()));

    if (r.conflicts.empty()) {
        out() << "merged cleanly (" << r.merged.notes.size() << " notes)\n";
        return 0;
    }
    out() << r.conflicts.size() << " conflict(s) — kept ours for each:\n";
    for (const Conflict& c : r.conflicts)
        out() << "  " << qs(c.key) << ": ours=" << qs(c.ours) << " theirs=" << qs(c.theirs)
              << "\n";
    out().flush();
    return 1;
}

int runExport(const QStringList& args)
{
    if (args.size() < 2) {
        out() << "usage: ovc-cli export <repoId|folder> <out.ovcz> [--text-only]\n";
        return 1;
    }
    ovc::git::Registry reg = ovc::git::Registry::load();
    const auto* entry = resolveEntry(reg, args.at(0));
    if (!entry) {
        out() << "not tracked: " << args.at(0) << "\n";
        return 1;
    }
    QString err;
    if (!ovc::git::exportBundle(*entry, args.at(1), args.contains("--text-only"), &err)) {
        out() << "export failed: " << err << "\n";
        return 1;
    }
    out() << "exported " << args.at(1) << " (" << QFileInfo(args.at(1)).size() / 1024
          << " KiB)\n";
    out().flush();
    return 0;
}

int runImport(const QStringList& args)
{
    if (args.isEmpty()) {
        out() << "usage: ovc-cli import <bundle.ovcz> [--into <songs folder>]\n";
        return 1;
    }
    QString into;
    const int intoIdx = int(args.indexOf("--into"));
    if (intoIdx >= 0 && intoIdx + 1 < args.size()) into = args.at(intoIdx + 1);
    QString err;
    const auto entry = ovc::git::importBundle(args.first(), into, &err);
    if (!entry) {
        out() << "import failed: " << err << "\n";
        return 1;
    }
    out() << "imported " << entry->artist << " - " << entry->title << "\n  repoId "
          << entry->repoId
          << (entry->songsPath.isEmpty() ? "\n  (view-only: no local folder linked)\n"
                                         : "\n  linked to " + entry->songsPath + "\n");
    out().flush();
    return 0;
}

int runMergeBundle(const QStringList& args)
{
    if (args.size() < 2) {
        out() << "usage: ovc-cli merge-bundle <repoId|folder> <bundle.ovcz>\n";
        return 1;
    }
    ovc::git::Registry reg = ovc::git::Registry::load();
    const auto* entry = resolveEntry(reg, args.at(0));
    if (!entry) {
        out() << "not tracked: " << args.at(0) << "\n";
        return 1;
    }
    QString err;
    const auto outcome = ovc::git::collabMergeBundle(*entry, args.at(1), &err);
    if (!outcome) {
        out() << "merge failed: " << (err.isEmpty() ? QStringLiteral("unknown error") : err)
              << "\n";
        return 1;
    }
    const auto& rep = outcome->report;
    if (!rep.anyChange()) {
        out() << "nothing to merge (bundle has no changes you're missing)\n";
        return 0;
    }
    for (const auto& f : rep.files) {
        if (f.wholeFileConflict)
            out() << "  [" << f.version << "] skipped — " << f.reason << "\n";
        else if (f.conflictCount() > 0) {
            out() << "  [" << f.version << "] " << f.conflictCount()
                  << " conflict(s), kept ours:\n";
            for (const QString& k : f.conflictKeys) out() << "      " << k << "\n";
        }
        else if (f.changed)
            out() << "  [" << f.version << "] merged cleanly\n";
    }
    out() << (rep.totalConflicts() == 0 ? "merged cleanly" : "merged with conflicts")
          << " — press F5 in song select before reopening.\n";
    out().flush();
    return rep.totalConflicts() == 0 ? 0 : 1;
}

int runServe(QCoreApplication& app)
{
    using namespace ovc::watch;
    const ovc::utils::Config cfg = ovc::utils::Config::load();
    out() << "serve: tracking + local API on http://127.0.0.1:" << cfg.serverPort
          << " (Ctrl+C to quit)\n"
          << "token: " << cfg.serverToken << "\n";
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

    ovc::git::MergeSessionStore merges;
    ovc::serve::LocalServer server(svc, merges, cfg);
    // Headless mode auto-approves restores (the CLI user asked for the server).
    server.setRestoreConfirmer(
        [](QString, QString) { return QtFuture::makeReadyValueFuture(true); });
    QString err;
    if (!server.start(&err)) {
        out() << "server failed: " << err << "\n";
        return 1;
    }
    QObject::connect(&svc, &TrackingService::snapshotTaken,
                     [](const QString&, const QString& subject, const QByteArray& oid) {
                         out() << "[snap] " << oid.left(7) << "  " << subject << "\n";
                         out().flush();
                     });
    return app.exec();
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
    if (cmd == "merge-file") return runMergeFile(args.mid(2));
    if (cmd == "track") return runTrack(args.mid(2));
    if (cmd == "list") return runList(args.mid(2));
    if (cmd == "untrack") return runUntrack(args.mid(2));
    if (cmd == "relink") return runRelink(args.mid(2));
    if (cmd == "snapshot") return runSnapshot(args.mid(2));
    if (cmd == "log") return runLog(args.mid(2));
    if (cmd == "fswatch") return runFswatch(args.mid(2));
    if (cmd == "watch") return runWatch(app);
    if (cmd == "serve") return runServe(app);
    if (cmd == "restore") return runRestore(args.mid(2));
    if (cmd == "export") return runExport(args.mid(2));
    if (cmd == "import") return runImport(args.mid(2));
    if (cmd == "merge-bundle") return runMergeBundle(args.mid(2));

    out() << "usage: ovc-cli <command>\n"
             "  probe                     stream osu! memory-reader state\n"
             "  watch                     full tracking loop: auto-snapshot the active mapset\n"
             "  gitcheck                  print libgit2 version and run a scratch-repo self test\n"
             "  parse --check <path> [-v] verify lossless round-trip of .osu file(s) or dir\n"
             "  diff <a.osu> <b.osu>      semantic diff of two difficulties\n"
             "  merge-file <base> <ours> <theirs>  3-way merge (writes ours; git driver)\n"
             "  track <folder>            start tracking a mapset folder (shadow repo + import)\n"
             "  list                      list tracked mapsets (repoId + name)\n"
             "  untrack <id|folder|title> [--keep-data]  stop tracking; deletes history unless --keep-data\n"
             "  relink <id|folder|title> <newFolder>     re-point a tracked set at a renamed/uploaded folder\n"
             "  snapshot <id|folder>      mirror + commit current state if changed\n"
             "  log <id|folder>           list snapshots of a tracked mapset\n"
             "  restore <id|folder> <oid> write an old snapshot back into the Songs folder\n"
             "  serve                     watch + local API for the web viewer\n"
             "  export <id|folder> <out.ovcz> [--text-only]  shareable history bundle\n"
             "  import <bundle.ovcz> [--into <folder>]       load a bundle into the registry\n"
             "  merge-bundle <id|folder> <bundle.ovcz>       merge a collaborator's bundle in\n"
             "  fswatch <folder>          log raw filesystem events (save-behavior spike)\n";
    out().flush();
    return cmd.isEmpty() ? 0 : 1;
}
