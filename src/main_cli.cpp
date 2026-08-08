#include <git/gitcheck.h>
#include <osu/parser.h>
#include <osu/serializer.h>
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

    out() << "usage: ovc-cli <command>\n"
             "  probe                     stream osu! memory-reader state\n"
             "  gitcheck                  print libgit2 version and run a scratch-repo self test\n"
             "  parse --check <path> [-v] verify lossless round-trip of .osu file(s) or dir\n";
    out().flush();
    return cmd.isEmpty() ? 0 : 1;
}
