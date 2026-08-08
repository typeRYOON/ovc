#include <git/gitcheck.h>
#include <osu/canonical.h>
#include <osu/diff.h>
#include <osu/parser.h>
#include <osu/serializer.h>
#include <utils/timefmt.h>
#include <watch/gamewatcher.h>
#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QTimer>

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

    out() << "usage: ovc-cli <command>\n"
             "  probe                     stream osu! memory-reader state\n"
             "  gitcheck                  print libgit2 version and run a scratch-repo self test\n"
             "  parse --check <path> [-v] verify lossless round-trip of .osu file(s) or dir\n"
             "  diff <a.osu> <b.osu>      semantic diff of two difficulties\n";
    out().flush();
    return cmd.isEmpty() ? 0 : 1;
}
