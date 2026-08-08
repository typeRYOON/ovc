#include <git/gitcheck.h>
#include <git/paths.h>
#include <git/shadowrepo.h>
#include <watch/trackingservice.h>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace ovc::watch;
using ovc::git::ShadowRepo;

namespace {

QByteArray osuFixture(int mapId, int setId, const char* version, const char* extraNote = "")
{
    return QByteArray("osu file format v14\n\n[General]\nMode: 3\n\n[Metadata]\nTitle:T\n"
                      "Artist:A\nCreator:C\nVersion:") +
           version + QByteArray("\nBeatmapID:") + QByteArray::number(mapId) +
           QByteArray("\nBeatmapSetID:") + QByteArray::number(setId) +
           QByteArray("\n\n[Difficulty]\nHPDrainRate:8\nCircleSize:7\nOverallDifficulty:8\n"
                      "ApproachRate:5\nSliderMultiplier:1.4\nSliderTickRate:1\n\n"
                      "[TimingPoints]\n1000,300,4,2,1,70,1,0\n\n[HitObjects]\n"
                      "36,192,1000,1,0,0:0:0:0:\n") +
           extraNote;
}

bool writeFile(const QString& path, const QByteArray& content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(content) == content.size();
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

MemBeatmap fakeMem(const QString& songsDir, const QString& folder, int mapId, int setId)
{
    MemBeatmap m;
    m.md5 = QStringLiteral("fake");
    m.folder = folder;
    m.songsDir = songsDir;
    m.filename = QStringLiteral("x.osu");
    m.osuPath = songsDir + '/' + folder + QStringLiteral("/x.osu");
    m.mapId = mapId;
    m.setId = setId;
    return m;
}

} // namespace

class TestPipeline : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void saveProducesOneCommit();
    void burstCollapsesToOneCommit();
    void noChangeRewriteNoCommit();
    void renameRebindsViaContentProbe();
    void preflightBlocksEditor();
    void restoreRoundTrip();

private:
    QTemporaryDir m_songs; // fake Songs root shared by tests
};

void TestPipeline::initTestCase()
{
    QCoreApplication::setApplicationName("ovc");
    QStandardPaths::setTestModeEnabled(true);
    static ovc::git::LibGit s_libgit;
    QDir(ovc::git::dataRoot()).removeRecursively();
    QVERIFY(m_songs.isValid());
}

void TestPipeline::saveProducesOneCommit()
{
    const QString folder = QStringLiteral("99 T - A");
    const QString dir = m_songs.path() + '/' + folder;
    QVERIFY(writeFile(dir + "/T - A (C) [VA].osu", osuFixture(11, 99, "VA")));
    QVERIFY(writeFile(dir + "/audio.mp3", QByteArray(5000, 'x')));

    TrackingService svc;
    svc.binder().setTimings(100, 30);
    QSignalSpy active(&svc, &TrackingService::activeMapsetChanged);
    QSignalSpy taken(&svc, &TrackingService::snapshotTaken);

    svc.onBeatmapChanged(fakeMem(m_songs.path(), folder, 11, 99));
    QCOMPARE(active.count(), 1);
    QVERIFY(active.last().at(0).toString().isEmpty()); // untracked yet

    QString err;
    const auto entry = svc.trackCurrentMapset(&err);
    QVERIFY2(entry.has_value(), qPrintable(err));
    QCOMPARE(svc.activeRepoId(), entry->repoId);
    QCOMPARE(svc.binder().boundDir(), QDir::cleanPath(dir));

    QSignalSpy stable(&svc.binder(), &SongsBinder::folderChangedStable);
    QTest::qWait(20);
    QVERIFY(writeFile(dir + "/T - A (C) [VA].osu",
                      osuFixture(11, 99, "VA", "256,192,1500,1,0,0:0:0:0:\n")));
    QTRY_VERIFY_WITH_TIMEOUT(stable.count() >= 1, 4000); // watcher chain fired
    QTRY_COMPARE_WITH_TIMEOUT(taken.count(), 1, 8000);
    QVERIFY(taken.last().at(1).toString().contains("+1"));

    auto repo = ShadowRepo::open(entry->repoDir());
    QCOMPARE(repo->log().size(), 2);
    QCOMPARE(repo->log()[0].trailers.value("Ovc-Trigger"), QStringLiteral("autosave"));
}

void TestPipeline::burstCollapsesToOneCommit()
{
    const QString folder = QStringLiteral("99 T - A");
    const QString dir = m_songs.path() + '/' + folder;

    TrackingService svc;
    svc.binder().setTimings(150, 30);
    QSignalSpy taken(&svc, &TrackingService::snapshotTaken);
    svc.onBeatmapChanged(fakeMem(m_songs.path(), folder, 11, 99));
    QVERIFY(!svc.activeRepoId().isEmpty()); // registry remembers it

    // Three rapid writes inside one debounce window → one snapshot.
    for (int i = 0; i < 3; ++i) {
        QVERIFY(writeFile(dir + "/T - A (C) [VA].osu",
                          osuFixture(11, 99, "VA",
                                     QByteArray("256,192,1500,1,0,0:0:0:0:\n475,192,") +
                                         QByteArray::number(1600 + i * 100) +
                                         ",1,0,0:0:0:0:\n")));
        QTest::qWait(40);
    }
    QTRY_COMPARE_WITH_TIMEOUT(taken.count(), 1, 8000);
    QTest::qWait(600); // no follow-up commit sneaks in
    QCOMPARE(taken.count(), 1);
}

void TestPipeline::noChangeRewriteNoCommit()
{
    const QString folder = QStringLiteral("99 T - A");
    const QString dir = m_songs.path() + '/' + folder;

    TrackingService svc;
    svc.binder().setTimings(100, 30);
    QSignalSpy taken(&svc, &TrackingService::snapshotTaken);
    QSignalSpy failed(&svc, &TrackingService::snapshotFailed);
    svc.onBeatmapChanged(fakeMem(m_songs.path(), folder, 11, 99));

    const QByteArray current = readFile(dir + "/T - A (C) [VA].osu");
    QVERIFY(!current.isEmpty());
    QTest::qWait(20);
    QVERIFY(writeFile(dir + "/T - A (C) [VA].osu", current)); // same bytes, fresh mtime

    QTest::qWait(1200); // binder fires, snapshot job runs, finds nothing
    QCOMPARE(taken.count(), 0);
    QCOMPARE(failed.count(), 0);
}

void TestPipeline::renameRebindsViaContentProbe()
{
    // Unsubmitted set (-1 ids): only the content probe can match it.
    const QString folder = QStringLiteral("wip_map");
    const QString dir = m_songs.path() + '/' + folder;
    QVERIFY(writeFile(dir + "/wip [A].osu", osuFixture(-1, -1, "A")));

    TrackingService svc;
    svc.binder().setTimings(100, 30);
    svc.onBeatmapChanged(fakeMem(m_songs.path(), folder, -1, -1));
    QString err;
    const auto entry = svc.trackCurrentMapset(&err);
    QVERIFY2(entry.has_value(), qPrintable(err));

    const QString newFolder = QStringLiteral("wip_map_renamed");
    QVERIFY(QDir(m_songs.path()).rename(folder, newFolder));

    QSignalSpy active(&svc, &TrackingService::activeMapsetChanged);
    svc.onBeatmapChanged(fakeMem(m_songs.path(), newFolder, -1, -1));
    // Same repo resolved through the probe: no activeMapsetChanged("") flap.
    QCOMPARE(svc.activeRepoId(), entry->repoId);
    QCOMPARE(active.count(), 0);

    ovc::git::Registry reg = ovc::git::Registry::load();
    const auto* e = reg.findByRepoId(entry->repoId);
    QVERIFY(e);
    QCOMPARE(QDir::cleanPath(e->songsPath), QDir::cleanPath(m_songs.path() + '/' + newFolder));
}

void TestPipeline::preflightBlocksEditor()
{
    const QString folder = QStringLiteral("99 T - A");
    TrackingService svc;
    svc.onBeatmapChanged(fakeMem(m_songs.path(), folder, 11, 99));
    const QString rid = svc.activeRepoId();
    QVERIFY(!rid.isEmpty());

    svc.onStateChanged(GameState::Edit);
    QVERIFY(!svc.preflightRestore(rid).allowed);
    QString err;
    QVERIFY(!svc.restore(rid, "deadbeef", &err).has_value());
    QVERIFY(!err.isEmpty());

    svc.onStateChanged(GameState::Menu);
    QVERIFY(svc.preflightRestore(rid).allowed);
}

void TestPipeline::restoreRoundTrip()
{
    const QString folder = QStringLiteral("99 T - A");
    const QString dir = m_songs.path() + '/' + folder;

    TrackingService svc;
    svc.binder().setTimings(100, 30);
    svc.onBeatmapChanged(fakeMem(m_songs.path(), folder, 11, 99));
    svc.onStateChanged(GameState::Menu);
    const QString rid = svc.activeRepoId();
    QVERIFY(!rid.isEmpty());

    ovc::git::Registry reg = ovc::git::Registry::load();
    auto repo = ShadowRepo::open(reg.findByRepoId(rid)->repoDir());
    const auto history = repo->log();
    const QByteArray importOid = history.last().oid; // oldest = import
    const QByteArray restoredContent = repo->readBlob(
        [&]() -> QByteArray {
            for (const auto& [p, b] : repo->listTree(importOid))
                if (p.endsWith(".osu")) return b;
            return {};
        }());
    QVERIFY(!restoredContent.isEmpty());

    QString err;
    const auto res = svc.restore(rid, importOid, &err);
    QVERIFY2(res.has_value(), qPrintable(err));
    QVERIFY(res->subject.startsWith("[restore]"));

    // Songs file is back to the imported content.
    QCOMPARE(readFile(dir + "/T - A (C) [VA].osu"), restoredContent);
    const auto log = repo->log();
    QCOMPARE(log[0].trailers.value("Ovc-Trigger"), QStringLiteral("restore"));
    QVERIFY(log[0].trailers.contains("Ovc-Restored-From"));
}

QTEST_GUILESS_MAIN(TestPipeline)
#include "test_pipeline.moc"
