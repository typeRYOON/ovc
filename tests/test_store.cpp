#include <git/gitcheck.h>
#include <git/mirror.h>
#include <git/ops.h>
#include <git/paths.h>
#include <git/registry.h>
#include <git/setdiff.h>
#include <git/shadowrepo.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace ovc::git;

namespace {

QByteArray osuFixture(int beatmapId, const char* version, const char* extraNote = "")
{
    return QByteArray("osu file format v14\n"
                      "\n"
                      "[General]\n"
                      "Mode: 3\n"
                      "\n"
                      "[Metadata]\n"
                      "Title:T\n"
                      "Artist:A\n"
                      "Creator:C\n"
                      "Version:") +
           version +
           QByteArray("\n"
                      "BeatmapID:") +
           QByteArray::number(beatmapId) +
           QByteArray("\n"
                      "BeatmapSetID:99\n"
                      "\n"
                      "[Difficulty]\n"
                      "HPDrainRate:8\n"
                      "CircleSize:7\n"
                      "OverallDifficulty:8\n"
                      "ApproachRate:5\n"
                      "SliderMultiplier:1.4\n"
                      "SliderTickRate:1\n"
                      "\n"
                      "[TimingPoints]\n"
                      "1000,300,4,2,1,70,1,0\n"
                      "\n"
                      "[HitObjects]\n"
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

} // namespace

class TestStore : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void createAndOpen();
    void commitNoopSuppression();
    void trailerRoundTrip();
    void mirrorManifestFastPath();
    void mirrorDeletesAndJunk();
    void listTreeReadBlob();
    void checkoutRestore();
    void diffTreesOps();
    void classifyPathCases();
    void trackAndSnapshotFlow();
    void snapshotLabels();
    void subjectLineWording();
    void refreshMetadataUnicode();
};

void TestStore::initTestCase()
{
    QCoreApplication::setApplicationName("ovc");
    QStandardPaths::setTestModeEnabled(true); // keep registry/repos out of real %LOCALAPPDATA%
    static LibGit s_libgit;
    QDir(dataRoot()).removeRecursively(); // clean slate between runs
}

void TestStore::createAndOpen()
{
    QTemporaryDir tmp;
    QString err;
    QVERIFY2(ShadowRepo::create(tmp.path(), &err), qPrintable(err));
    QVERIFY(QFile::exists(tmp.path() + "/.gitattributes"));
    QVERIFY(readFile(tmp.path() + "/.gitattributes").contains("merge=osu"));

    auto repo = ShadowRepo::open(tmp.path());
    QVERIFY(repo.has_value());
    QVERIFY(repo->headOid().isEmpty());
    QVERIFY(repo->log().isEmpty());
}

void TestStore::commitNoopSuppression()
{
    QTemporaryDir tmp;
    QVERIFY(ShadowRepo::create(tmp.path(), nullptr));
    QVERIFY(writeFile(tmp.path() + "/a.txt", "hello"));
    auto repo = ShadowRepo::open(tmp.path());

    const auto first = repo->commitAll("first", {});
    QVERIFY(first.has_value());
    QCOMPARE(repo->headOid(), *first);

    QVERIFY(!repo->commitAll("again", {}).has_value()); // unchanged tree

    // Same content, fresh mtime: still a no-op commit-wise.
    QVERIFY(writeFile(tmp.path() + "/a.txt", "hello"));
    QVERIFY(!repo->commitAll("again", {}).has_value());

    QVERIFY(writeFile(tmp.path() + "/a.txt", "changed"));
    const auto second = repo->commitAll("second", {});
    QVERIFY(second.has_value());
    const auto log = repo->log();
    QCOMPARE(log.size(), 2);
    QCOMPARE(log[0].oid, *second);
    QCOMPARE(log[0].parentOid, *first);
    QVERIFY(log[1].parentOid.isEmpty());
}

void TestStore::trailerRoundTrip()
{
    QTemporaryDir tmp;
    QVERIFY(ShadowRepo::create(tmp.path(), nullptr));
    QVERIFY(writeFile(tmp.path() + "/a.txt", "x"));
    auto repo = ShadowRepo::open(tmp.path());

    const QMap<QString, QString> trailers = {
        {"Ovc-Trigger", "manual"},
        {"Ovc-Difficulty", "Lagrange Blossom"},
        {"Ovc-Time-Range", "41738-60685"},
    };
    QVERIFY(repo->commitAll("subject line", trailers).has_value());
    const auto log = repo->log();
    QCOMPARE(log.size(), 1);
    QCOMPARE(log[0].subject, QStringLiteral("subject line"));
    QCOMPARE(log[0].trailers, trailers);
}

void TestStore::mirrorManifestFastPath()
{
    QTemporaryDir src, dst;
    QVERIFY(ShadowRepo::create(dst.path(), nullptr));
    QVERIFY(writeFile(src.path() + "/audio.mp3", QByteArray(300000, 'x')));
    QVERIFY(writeFile(src.path() + "/map.osu", osuFixture(11, "VA")));

    MirrorStats s1;
    QVERIFY(mirrorIntoRepo(src.path(), dst.path(), &s1, nullptr));
    QCOMPARE(s1.copied, 2);
    QCOMPARE(s1.statSkipped, 0);

    MirrorStats s2;
    QVERIFY(mirrorIntoRepo(src.path(), dst.path(), &s2, nullptr));
    QCOMPARE(s2.copied, 0); // the mp3 was not re-read
    QCOMPARE(s2.statSkipped, 2);

    QTest::qWait(10); // ensure a distinct mtime
    QVERIFY(writeFile(src.path() + "/map.osu", osuFixture(11, "VA", "256,192,1500,1,0,0:0:0:0:\n")));
    MirrorStats s3;
    QVERIFY(mirrorIntoRepo(src.path(), dst.path(), &s3, nullptr));
    QCOMPARE(s3.copied, 1);
    QCOMPARE(s3.statSkipped, 1);
}

void TestStore::mirrorDeletesAndJunk()
{
    QTemporaryDir src, dst;
    QVERIFY(ShadowRepo::create(dst.path(), nullptr));
    QVERIFY(writeFile(src.path() + "/keep.txt", "k"));
    QVERIFY(writeFile(src.path() + "/gone.txt", "g"));
    QVERIFY(writeFile(src.path() + "/junk.tmp", "j"));
    QVERIFY(writeFile(src.path() + "/sb/bg.jpg", "img"));

    MirrorStats s1;
    QVERIFY(mirrorIntoRepo(src.path(), dst.path(), &s1, nullptr));
    QCOMPARE(s1.copied, 3); // junk.tmp skipped
    QVERIFY(!QFile::exists(dst.path() + "/junk.tmp"));
    QVERIFY(QFile::exists(dst.path() + "/sb/bg.jpg"));

    QVERIFY(QFile::remove(src.path() + "/gone.txt"));
    MirrorStats s2;
    QVERIFY(mirrorIntoRepo(src.path(), dst.path(), &s2, nullptr));
    QCOMPARE(s2.deleted, 1);
    QVERIFY(!QFile::exists(dst.path() + "/gone.txt"));
    QVERIFY(QFile::exists(dst.path() + "/.gitattributes")); // reserved files untouched
}

void TestStore::listTreeReadBlob()
{
    QTemporaryDir tmp;
    QVERIFY(ShadowRepo::create(tmp.path(), nullptr));
    QVERIFY(writeFile(tmp.path() + "/a.txt", "alpha"));
    QVERIFY(writeFile(tmp.path() + "/sub/b.txt", "beta"));
    auto repo = ShadowRepo::open(tmp.path());
    const auto oid = repo->commitAll("c", {});
    QVERIFY(oid.has_value());

    const auto entries = repo->listTree(*oid);
    QHash<QString, QByteArray> byPath;
    for (const auto& [p, blob] : entries) byPath.insert(p, blob);
    QVERIFY(byPath.contains("a.txt"));
    QVERIFY(byPath.contains("sub/b.txt"));
    QCOMPARE(repo->readBlob(byPath["a.txt"]), QByteArray("alpha"));
    QCOMPARE(repo->readBlob(byPath["sub/b.txt"]), QByteArray("beta"));
    QCOMPARE(repo->blobSize(byPath["sub/b.txt"]), qint64(4));
}

void TestStore::checkoutRestore()
{
    QTemporaryDir tmp;
    QVERIFY(ShadowRepo::create(tmp.path(), nullptr));
    QVERIFY(writeFile(tmp.path() + "/a.txt", "v1"));
    auto repo = ShadowRepo::open(tmp.path());
    const auto c1 = repo->commitAll("v1", {});
    QVERIFY(c1.has_value());
    const QByteArray tree1 = repo->headTreeOid();

    QVERIFY(writeFile(tmp.path() + "/a.txt", "v2"));
    QVERIFY(writeFile(tmp.path() + "/new.txt", "n"));
    QVERIFY(repo->commitAll("v2", {}).has_value());

    QString err;
    QVERIFY2(repo->checkoutTree(*c1, &err), qPrintable(err));
    QCOMPARE(readFile(tmp.path() + "/a.txt"), QByteArray("v1"));
    QVERIFY(!QFile::exists(tmp.path() + "/new.txt")); // untracked-in-target removed

    // Restore = the old tree as a NEW commit; history stays linear.
    const auto c3 = repo->commitAll("restore v1", {});
    QVERIFY(c3.has_value());
    QCOMPARE(repo->headTreeOid(), tree1);
    QCOMPARE(repo->log().size(), 3);
}

void TestStore::diffTreesOps()
{
    QTemporaryDir tmp;
    QVERIFY(ShadowRepo::create(tmp.path(), nullptr));
    QVERIFY(writeFile(tmp.path() + "/T - A (C) [VA].osu", osuFixture(11, "VA")));
    QVERIFY(writeFile(tmp.path() + "/audio.mp3", QByteArray(1000, 'x')));
    QVERIFY(writeFile(tmp.path() + "/gone.wav", "w"));
    auto repo = ShadowRepo::open(tmp.path());
    const auto c1 = repo->commitAll("c1", {});
    QVERIFY(c1.has_value());

    // Rename the .osu (Version change → new filename), edit its content,
    // modify media, remove one file, add another.
    QVERIFY(QFile::remove(tmp.path() + "/T - A (C) [VA].osu"));
    QVERIFY(writeFile(tmp.path() + "/T - A (C) [VB].osu",
                      osuFixture(11, "VB", "256,192,1500,1,0,0:0:0:0:\n")));
    QVERIFY(writeFile(tmp.path() + "/audio.mp3", QByteArray(1001, 'y')));
    QVERIFY(QFile::remove(tmp.path() + "/gone.wav"));
    QVERIFY(writeFile(tmp.path() + "/bg.png", "img"));
    const auto c2 = repo->commitAll("c2", {});
    QVERIFY(c2.has_value());

    const SetDiff d = diffTrees(*repo, *c1, *c2);
    QCOMPARE(d.files.size(), 4);

    const FileChange* renamed = nullptr;
    const FileChange* audio = nullptr;
    const FileChange* wav = nullptr;
    const FileChange* png = nullptr;
    for (const FileChange& c : d.files) {
        if (c.kind == FileKind::Difficulty) renamed = &c;
        if (c.relPath == "audio.mp3") audio = &c;
        if (c.relPath == "gone.wav") wav = &c;
        if (c.relPath == "bg.png") png = &c;
    }
    QVERIFY(renamed && audio && wav && png);
    QCOMPARE(renamed->op, FileOp::Renamed);
    QCOMPARE(renamed->oldRelPath, QStringLiteral("T - A (C) [VA].osu"));
    QVERIFY(renamed->semantic.has_value());
    QCOMPARE(renamed->semantic->notes.size(), 1); // the added note survived the rename
    QCOMPARE(audio->op, FileOp::Modified);
    QCOMPARE(audio->kind, FileKind::Audio);
    QCOMPARE(wav->op, FileOp::Removed);
    QCOMPARE(wav->kind, FileKind::Sample);
    QCOMPARE(png->op, FileOp::Added);
    QCOMPARE(png->kind, FileKind::Image);

    const QString subject = d.subjectLine();
    QVERIFY(subject.contains("VB"));
    // Few media files → named individually, not a bare "media ~N".
    QVERIFY(subject.contains("audio.mp3 changed"));
    QVERIFY(subject.contains("bg.png added"));
    QVERIFY(subject.contains("gone.wav removed"));

    // Empty-tree base: everything shows as Added.
    const SetDiff import = diffTrees(*repo, "", *c1);
    QCOMPARE(import.files.size(), 3);
    for (const FileChange& c : import.files) QCOMPARE(c.op, FileOp::Added);
}

void TestStore::classifyPathCases()
{
    QCOMPARE(classifyPath(u"x [Hard].osu"), FileKind::Difficulty);
    QCOMPARE(classifyPath(u"map.osb"), FileKind::Storyboard);
    QCOMPARE(classifyPath(u"audio.mp3"), FileKind::Audio);
    QCOMPARE(classifyPath(u"audio.ogg"), FileKind::Audio);
    QCOMPARE(classifyPath(u"sb/loop.ogg"), FileKind::Sample);
    QCOMPARE(classifyPath(u"soft-hitclap.wav"), FileKind::Sample);
    QCOMPARE(classifyPath(u"sb/bg.JPG"), FileKind::Image);
    QCOMPARE(classifyPath(u"video.mp4"), FileKind::Video);
    QCOMPARE(classifyPath(u"readme.txt"), FileKind::Other);
}

void TestStore::trackAndSnapshotFlow()
{
    QTemporaryDir songs;
    const QString mapset = songs.path() + "/99 T - A";
    QVERIFY(writeFile(mapset + "/T - A (C) [VA].osu", osuFixture(11, "VA")));
    QVERIFY(writeFile(mapset + "/T - A (C) [VB].osu", osuFixture(12, "VB")));
    QVERIFY(writeFile(mapset + "/audio.mp3", QByteArray(5000, 'x')));

    QString err;
    const auto entry = trackMapset(mapset, &err);
    QVERIFY2(entry.has_value(), qPrintable(err));
    QCOMPARE(entry->beatmapSetId, 99);
    QCOMPARE(entry->beatmapIds.size(), 2);
    QCOMPARE(entry->creator, QStringLiteral("C"));

    // Registry persisted and resolvable.
    Registry reg = Registry::load();
    QVERIFY(reg.findByRepoId(entry->repoId));
    QVERIFY(reg.findBySetId(99));
    QVERIFY(reg.findByBeatmapId(12));
    QVERIFY(reg.findBySongsPath(mapset));

    // Double-track refused.
    QVERIFY(!trackMapset(mapset, &err).has_value());
    QVERIFY(err.contains("already tracked"));

    auto repo = ShadowRepo::open(entry->repoDir());
    QVERIFY(repo.has_value());
    auto log = repo->log();
    QCOMPARE(log.size(), 1);
    QVERIFY(log[0].subject.startsWith("[import]"));
    QCOMPARE(log[0].trailers.value("Ovc-Trigger"), QStringLiteral("import"));

    // No-change snapshot: clean no-op.
    const auto none = snapshotMapset(*entry, "manual", {}, &err);
    QVERIFY2(!none.has_value(),
             qPrintable(QStringLiteral("unexpected commit: %1").arg(none ? none->subject : "")));
    QVERIFY(err.isEmpty());

    // Real change → commit with semantic subject + trailers.
    QTest::qWait(10);
    QVERIFY(writeFile(mapset + "/T - A (C) [VA].osu",
                      osuFixture(11, "VA", "256,192,1500,128,0,2500:0:0:0:0:\n")));
    const auto snap = snapshotMapset(*entry, "autosave", {}, &err);
    QVERIFY2(snap.has_value(), qPrintable(err));
    QVERIFY(snap->subject.startsWith("[auto] "));
    QVERIFY(snap->subject.contains("VA"));
    QVERIFY(snap->subject.contains("+1"));

    log = repo->log();
    QCOMPARE(log.size(), 2);
    QCOMPARE(log[0].trailers.value("Ovc-Trigger"), QStringLiteral("autosave"));
    QCOMPARE(log[0].trailers.value("Ovc-Difficulty"), QStringLiteral("VA"));
    QCOMPARE(log[0].trailers.value("Ovc-Time-Range"), QStringLiteral("1500-2500"));
    QVERIFY(log[0].trailers.contains("Ovc-Files"));
}

void TestStore::snapshotLabels()
{
    QTemporaryDir tmp;
    QVERIFY(ShadowRepo::create(tmp.path(), nullptr));
    QVERIFY(writeFile(tmp.path() + "/a.txt", "v1"));
    auto repo = ShadowRepo::open(tmp.path());
    const auto c1 = repo->commitAll("first", {});
    QVERIFY(writeFile(tmp.path() + "/a.txt", "v2"));
    const auto c2 = repo->commitAll("second", {});
    QVERIFY(c1 && c2);

    // No label until set; setting one shows up in log() and commitInfo().
    QVERIFY(repo->labelFor(*c1).isEmpty());
    QVERIFY(repo->setLabel(*c1, QStringLiteral("  before rework  ")));
    QCOMPARE(repo->labelFor(*c1), QStringLiteral("before rework")); // trimmed
    QCOMPARE(repo->commitInfo(*c1)->label, QStringLiteral("before rework"));

    auto log = repo->log();
    QCOMPARE(log.size(), 2);
    // log is newest-first: c2 unlabeled, c1 labeled.
    QVERIFY(log[0].label.isEmpty());
    QCOMPARE(log[1].label, QStringLiteral("before rework"));

    // Labels are keyed by oid and don't change it — restore-by-hash still works.
    QCOMPARE(repo->headOid(), *c2);

    // Empty name clears; a second label coexists.
    QVERIFY(repo->setLabel(*c2, QStringLiteral("final")));
    QVERIFY(repo->setLabel(*c1, QString()));
    QVERIFY(repo->labelFor(*c1).isEmpty());
    QCOMPARE(repo->labelFor(*c2), QStringLiteral("final"));

    // Persists across reopen (it's a file beside the repo).
    auto reopened = ShadowRepo::open(tmp.path());
    QCOMPARE(reopened->labelFor(*c2), QStringLiteral("final"));
}

void TestStore::subjectLineWording()
{
    QTemporaryDir tmp;
    QVERIFY(ShadowRepo::create(tmp.path(), nullptr));
    QVERIFY(writeFile(tmp.path() + "/audio.mp3", QByteArray(100, 'a')));
    auto repo = ShadowRepo::open(tmp.path());
    const auto c1 = repo->commitAll("c1", {});
    QVERIFY(c1.has_value());

    // Single media edit names the file — no opaque "media ~1".
    QVERIFY(writeFile(tmp.path() + "/audio.mp3", QByteArray(200, 'b')));
    const auto c2 = repo->commitAll("c2", {});
    QString subject = diffTrees(*repo, *c1, *c2).subjectLine();
    QCOMPARE(subject, QStringLiteral("audio.mp3 changed"));

    // Many media files fall back to a count with a breakdown.
    for (int i = 0; i < 5; ++i)
        QVERIFY(writeFile(tmp.path() + QStringLiteral("/s%1.wav").arg(i), QByteArray(50, 'x')));
    const auto c3 = repo->commitAll("c3", {});
    subject = diffTrees(*repo, *c2, *c3).subjectLine();
    QVERIFY2(subject.contains("5 media files"), qPrintable(subject));
    QVERIFY(subject.contains("+5"));
}

void TestStore::refreshMetadataUnicode()
{
    // 陽だまりの詩 / ゆよゆっぺ as raw UTF-8, spelled in hex so the check is
    // independent of this source file's own encoding. A regression guard for the
    // bug where a set's Japanese title/artist stored as CP1252-mangled mojibake.
    const QByteArray titleUtf8 =
        "\xE9\x99\xBD\xE3\x81\xA0\xE3\x81\xBE\xE3\x82\x8A\xE3\x81\xAE\xE8\xA9\xA9";
    const QByteArray artistUtf8 = "\xE3\x82\x86\xE3\x82\x88\xE3\x82\x86\xE3\x81\xA3\xE3\x81\xBA";

    QTemporaryDir songs;
    const QByteArray osu = QByteArray("osu file format v14\n\n[Metadata]\n"
                                      "Title:Hidamari no Uta\nTitleUnicode:") +
                           titleUtf8 + "\nArtist:Yuyoyuppe\nArtistUnicode:" + artistUtf8 +
                           "\nCreator:Undead Alice\nBeatmapID:1\nBeatmapSetID:2\n";
    QVERIFY(writeFile(songs.path() + "/map.osu", osu));

    MapsetEntry e;
    e.songsPath = songs.path();
    e.title = QStringLiteral("stale");
    e.artist = QStringLiteral("stale");

    QVERIFY(refreshMapsetMetadata(e));
    // Native Unicode metadata is preferred and decoded as UTF-8, not the local codepage.
    QCOMPARE(e.title.toUtf8(), titleUtf8);
    QCOMPARE(e.artist.toUtf8(), artistUtf8);
    QCOMPARE(e.creator, QStringLiteral("Undead Alice"));

    // And it survives the registry's JSON serialization (UTF-8) with no re-encoding.
    const QByteArray json =
        QJsonDocument(QJsonObject{{QStringLiteral("title"), e.title}}).toJson();
    const QString roundTrip =
        QJsonDocument::fromJson(json).object().value(QStringLiteral("title")).toString();
    QCOMPARE(roundTrip.toUtf8(), titleUtf8);

    QVERIFY(!refreshMapsetMetadata(e)); // idempotent: a second pass is a no-op
}

QTEST_GUILESS_MAIN(TestStore)
#include "test_store.moc"
