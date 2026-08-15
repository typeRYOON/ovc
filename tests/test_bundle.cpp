#include <git/bundle.h>
#include <git/gitcheck.h>
#include <git/ops.h>
#include <git/paths.h>
#include <git/shadowrepo.h>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace ovc::git;

namespace {

QByteArray osuFixture(const char* extra = "")
{
    return QByteArray("osu file format v14\n\n[General]\nMode: 3\nAudioFilename: audio.mp3\n\n"
                      "[Metadata]\nTitle:T\nArtist:A\nCreator:C\nVersion:VA\nBeatmapID:11\n"
                      "BeatmapSetID:99\n\n[Difficulty]\nHPDrainRate:8\nCircleSize:7\n"
                      "OverallDifficulty:8\nApproachRate:5\nSliderMultiplier:1.4\n"
                      "SliderTickRate:1\n\n[TimingPoints]\n1000,300,4,2,1,70,1,0\n\n"
                      "[HitObjects]\n36,192,1000,1,0,0:0:0:0:\n") +
           extra;
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

class TestBundle : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void fullRoundTrip();
    void labelsTravel();
    void textOnlySkipsMedia();
    void importedRestoreNeedsTarget();
    void collabMergeDisjointEdits();
    void collabSyncsMedia();
    void resolveMergePicksTheirs();

private:
    QTemporaryDir m_songs;
    MapsetEntry m_entry;
};

void TestBundle::initTestCase()
{
    QCoreApplication::setApplicationName("ovc");
    QStandardPaths::setTestModeEnabled(true);
    static LibGit s_libgit;
    QDir(dataRoot()).removeRecursively();

    const QString mapset = m_songs.path() + "/99 T - A";
    QVERIFY(writeFile(mapset + "/T - A (C) [VA].osu", osuFixture()));
    // Incompressible fake media, like real mp3 — the text-only size assertion
    // is meaningless against a run of identical bytes.
    QByteArray media(200000, Qt::Uninitialized);
    QRandomGenerator gen(42);
    for (auto& byte : media) byte = char(gen.bounded(256));
    QVERIFY(writeFile(mapset + "/audio.mp3", media));
    QString err;
    const auto entry = trackMapset(mapset, &err);
    QVERIFY2(entry.has_value(), qPrintable(err));
    m_entry = *entry;

    QTest::qWait(15);
    QVERIFY(writeFile(mapset + "/T - A (C) [VA].osu",
                      osuFixture("256,192,1500,128,0,2500:0:0:0:0:\n")));
    QVERIFY(snapshotMapset(m_entry, "manual", {}, &err).has_value());
}

void TestBundle::fullRoundTrip()
{
    QTemporaryDir tmp;
    const QString path = tmp.path() + "/set.ovcz";
    QString err;
    QVERIFY2(exportBundle(m_entry, path, false, &err), qPrintable(err));

    const auto info = peekBundle(path, &err);
    QVERIFY2(info.has_value(), qPrintable(err));
    QCOMPARE(info->title, QStringLiteral("T"));
    QCOMPARE(info->snapshotCount, 2);
    QVERIFY(!info->textOnly);

    const auto imported = importBundle(path, {}, &err);
    QVERIFY2(imported.has_value(), qPrintable(err));
    QVERIFY(imported->repoId != m_entry.repoId);
    QVERIFY(imported->songsPath.isEmpty());
    QVERIFY(!imported->autoSnapshot);

    auto src = ShadowRepo::open(m_entry.repoDir());
    auto dst = ShadowRepo::open(imported->repoDir());
    QVERIFY(src && dst);
    const auto srcLog = src->log();
    const auto dstLog = dst->log();
    QCOMPARE(dstLog.size(), srcLog.size());
    for (int i = 0; i < srcLog.size(); ++i) {
        QCOMPARE(dstLog[i].subject, srcLog[i].subject);
        QCOMPARE(dstLog[i].when, srcLog[i].when);
        QCOMPARE(dstLog[i].trailers.value("Ovc-Bundle-Oid"),
                 QString::fromUtf8(srcLog[i].oid));
    }

    // Blob content survives: the .osu at HEAD matches byte-for-byte.
    QByteArray srcOsu, dstOsu;
    for (const auto& [p, b] : src->listTree(src->headOid()))
        if (p.endsWith(".osu")) srcOsu = src->readBlob(b);
    for (const auto& [p, b] : dst->listTree(dst->headOid()))
        if (p.endsWith(".osu")) dstOsu = dst->readBlob(b);
    QVERIFY(!srcOsu.isEmpty());
    QCOMPARE(dstOsu, srcOsu);
}

void TestBundle::labelsTravel()
{
    // Label the newest snapshot on the source, export, import, and confirm the
    // label re-attaches to the corresponding (freshly-minted) commit.
    auto src = ShadowRepo::open(m_entry.repoDir());
    const QByteArray head = src->headOid();
    QVERIFY(src->setLabel(head, QStringLiteral("kiai redone")));

    QTemporaryDir tmp;
    const QString path = tmp.path() + "/set.ovcz";
    QString err;
    QVERIFY(exportBundle(m_entry, path, false, &err));
    const auto imported = importBundle(path, {}, &err);
    QVERIFY2(imported.has_value(), qPrintable(err));

    auto dst = ShadowRepo::open(imported->repoDir());
    const auto log = dst->log();
    QCOMPARE(log.size(), 2);
    QCOMPARE(log[0].label, QStringLiteral("kiai redone")); // newest carries it
    QVERIFY(log[1].label.isEmpty());

    src->setLabel(head, QString()); // don't leak into later tests
}

void TestBundle::textOnlySkipsMedia()
{
    QTemporaryDir tmp;
    const QString full = tmp.path() + "/full.ovcz";
    const QString text = tmp.path() + "/text.ovcz";
    QString err;
    QVERIFY(exportBundle(m_entry, full, false, &err));
    QVERIFY(exportBundle(m_entry, text, true, &err));
    QVERIFY(QFileInfo(text).size() < QFileInfo(full).size() / 4); // 200KB mp3 skipped

    const auto imported = importBundle(text, {}, &err);
    QVERIFY2(imported.has_value(), qPrintable(err));
    auto repo = ShadowRepo::open(imported->repoDir());
    bool hasOsu = false, hasMedia = false;
    for (const auto& [p, b] : repo->listTree(repo->headOid())) {
        if (p.endsWith(".osu")) hasOsu = true;
        if (p.endsWith(".mp3")) hasMedia = true;
    }
    QVERIFY(hasOsu);
    QVERIFY(!hasMedia); // media data absent from a text-only bundle
}

void TestBundle::importedRestoreNeedsTarget()
{
    QTemporaryDir tmp;
    const QString path = tmp.path() + "/set.ovcz";
    QString err;
    QVERIFY(exportBundle(m_entry, path, false, &err));
    const auto viewOnly = importBundle(path, {}, &err);
    QVERIFY(viewOnly.has_value());
    auto repo = ShadowRepo::open(viewOnly->repoDir());
    const QByteArray oid = repo->log().last().oid;
    QVERIFY(!restoreMapset(*viewOnly, oid, &err).has_value());
    QVERIFY(err.contains("no local folder"));

    // With --into, restore materializes the snapshot into the target folder.
    QTemporaryDir target;
    const auto linked = importBundle(path, target.path() + "/restored", &err);
    QVERIFY2(linked.has_value(), qPrintable(err));
    auto repo2 = ShadowRepo::open(linked->repoDir());
    const QByteArray first = repo2->log().last().oid;
    const auto res = restoreMapset(*linked, first, &err);
    QVERIFY2(res.has_value(), qPrintable(err));
    QVERIFY(QFile::exists(target.path() + "/restored/T - A (C) [VA].osu"));
}

void TestBundle::collabMergeDisjointEdits()
{
    // Two mappers fork a shared base and edit different things; merging the
    // collaborator's bundle should combine both into ours' Songs folder.
    QTemporaryDir songs;
    const QString mapset = songs.path() + "/collab";
    const QString osu = mapset + "/T - A (C) [VA].osu";
    QVERIFY(writeFile(osu, osuFixture()));               // shared base
    QVERIFY(writeFile(mapset + "/audio.mp3", QByteArray(1000, 'x')));

    QString err;
    const auto ours = trackMapset(mapset, &err); // ours' repo, starting at base
    QVERIFY2(ours.has_value(), qPrintable(err));

    // The collaborator (a separate repo) starts from the same base, changes OD.
    QTemporaryDir theirSongs;
    const QString theirMapset = theirSongs.path() + "/collab";
    QVERIFY(writeFile(theirMapset + "/T - A (C) [VA].osu", osuFixture()));
    QVERIFY(writeFile(theirMapset + "/audio.mp3", QByteArray(1000, 'x')));
    const auto theirs = trackMapset(theirMapset, &err);
    QVERIFY(theirs.has_value());
    QTest::qWait(15);
    QByteArray theirEdit = osuFixture();
    theirEdit.replace("OverallDifficulty:8", "OverallDifficulty:6"); // theirs: OD 6
    QVERIFY(writeFile(theirMapset + "/T - A (C) [VA].osu", theirEdit));
    QVERIFY(snapshotMapset(*theirs, "manual", {}, &err).has_value());
    const QString bundle = theirSongs.path() + "/theirs.ovcz";
    QVERIFY(exportBundle(*theirs, bundle, false, &err));

    // Meanwhile ours adds a note (disjoint from theirs' OD change).
    QTest::qWait(15);
    QByteArray ourEdit = osuFixture("448,192,3000,1,0,0:0:0:0:\n"); // ours: add col3 note
    QVERIFY(writeFile(osu, ourEdit));

    // Merge theirs into ours.
    const auto outcome = collabMergeBundle(*ours, bundle, &err);
    QVERIFY2(outcome.has_value(), qPrintable(err));
    QVERIFY(outcome->report.anyChange());
    QCOMPARE(outcome->report.totalConflicts(), 0); // disjoint → clean
    QVERIFY(!outcome->snapshotOid.isEmpty());

    // ours' Songs .osu now carries BOTH edits.
    const QByteArray merged = readFile(osu);
    QVERIFY(merged.contains("OverallDifficulty:6"));       // theirs' change
    QVERIFY(merged.contains("448,192,3000,1,0,0:0:0:0:"));  // ours' added note

    // A merge snapshot exists with the source trailer.
    auto repo = ShadowRepo::open(ours->repoDir());
    const auto log = repo->log();
    QCOMPARE(log[0].trailers.value("Ovc-Trigger"), QStringLiteral("merge"));
    QVERIFY(log[0].subject.contains("clean"));

    // Now a conflicting edit: ours changes OD too, differently.
    QByteArray ourOd = osuFixture();
    ourOd.replace("OverallDifficulty:8", "OverallDifficulty:9");
    QVERIFY(writeFile(osu, ourOd));
    const auto conflicted = collabMergeBundle(*ours, bundle, &err);
    QVERIFY2(conflicted.has_value(), qPrintable(err));
    QCOMPARE(conflicted->report.totalConflicts(), 1);
    QVERIFY(conflicted->report.files[0].conflictKeys[0].contains("OverallDifficulty"));
    // Ours (9) is kept in the file.
    QVERIFY(readFile(osu).contains("OverallDifficulty:9"));
}

void TestBundle::resolveMergePicksTheirs()
{
    // The web-resolver path: a real overlap is held pending, then resolved by
    // the *choice* (theirs) the site sends back — never file content. The
    // desktop re-merges natively and is the sole writer of the Songs file.
    QTemporaryDir songs;
    const QString mapset = songs.path() + "/collab";
    const QString osu = mapset + "/T - A (C) [VA].osu";
    QVERIFY(writeFile(osu, osuFixture()));
    QVERIFY(writeFile(mapset + "/audio.mp3", QByteArray(1000, 'x')));
    QString err;
    const auto ours = trackMapset(mapset, &err);
    QVERIFY2(ours.has_value(), qPrintable(err));

    // Collaborator forks the same base and lowers OD to 6.
    QTemporaryDir theirSongs;
    const QString theirMapset = theirSongs.path() + "/collab";
    QVERIFY(writeFile(theirMapset + "/T - A (C) [VA].osu", osuFixture()));
    QVERIFY(writeFile(theirMapset + "/audio.mp3", QByteArray(1000, 'x')));
    const auto theirs = trackMapset(theirMapset, &err);
    QVERIFY(theirs.has_value());
    QTest::qWait(15);
    QByteArray theirEdit = osuFixture();
    theirEdit.replace("OverallDifficulty:8", "OverallDifficulty:6");
    QVERIFY(writeFile(theirMapset + "/T - A (C) [VA].osu", theirEdit));
    QVERIFY(snapshotMapset(*theirs, "manual", {}, &err).has_value());
    const QString bundle = theirSongs.path() + "/theirs.ovcz";
    QVERIFY(exportBundle(*theirs, bundle, false, &err));

    // Ours edits the same field differently → a genuine conflict.
    QTest::qWait(15);
    QByteArray ourEdit = osuFixture();
    ourEdit.replace("OverallDifficulty:8", "OverallDifficulty:9");
    QVERIFY(writeFile(osu, ourEdit));

    // prepare writes nothing; it just surfaces the conflict with a stable id.
    const auto prepared = prepareBundleMerge(*ours, bundle, &err);
    QVERIFY2(prepared.has_value(), qPrintable(err));
    QVERIFY(prepared->hasConflicts());
    QCOMPARE(prepared->totalConflicts(), 1);
    QVERIFY(readFile(osu).contains("OverallDifficulty:9")); // untouched on disk
    const QString cid = prepared->files.first().conflicts.first().id;
    const QString relPath = prepared->files.first().relPath;
    QCOMPARE(cid, QStringLiteral("kv:Difficulty:OverallDifficulty"));

    // The site sends { relPath: { id: "theirs" } }. Desktop re-merges + writes.
    const auto outcome =
        applyBundleMerge(*ours, *prepared, {{relPath, {{cid, QStringLiteral("theirs")}}}}, &err);
    QVERIFY2(outcome.has_value(), qPrintable(err));
    QCOMPARE(outcome->report.totalConflicts(), 1);
    QVERIFY(readFile(osu).contains("OverallDifficulty:6"));  // theirs won
    QVERIFY(!readFile(osu).contains("OverallDifficulty:9"));

    // Reversible + auditable: a pre-merge snapshot holds ours' OD9, and the
    // merge commit records that the conflict was resolved (not silently kept).
    auto repo = ShadowRepo::open(ours->repoDir());
    const auto log = repo->log();
    QVERIFY(log[0].subject.contains("resolved 1"));
    bool foundPre = false;
    for (const auto& c : log)
        if (c.trailers.value("Ovc-Trigger") == QStringLiteral("pre-merge")) foundPre = true;
    QVERIFY(foundPre);
}

void TestBundle::collabSyncsMedia()
{
    // A collaborator adds a hitsample and updates the audio; merging their
    // bundle pulls both into ours' Songs folder (media the mapper was missing).
    QTemporaryDir songs;
    const QString mapset = songs.path() + "/collab";
    const QString osu = mapset + "/T - A (C) [VA].osu";
    QVERIFY(writeFile(osu, osuFixture()));
    QVERIFY(writeFile(mapset + "/audio.mp3", QByteArray(1000, 'x'))); // shared base audio
    QString err;
    const auto ours = trackMapset(mapset, &err);
    QVERIFY2(ours.has_value(), qPrintable(err));

    QTemporaryDir theirSongs;
    const QString theirMapset = theirSongs.path() + "/collab";
    QVERIFY(writeFile(theirMapset + "/T - A (C) [VA].osu", osuFixture()));
    QVERIFY(writeFile(theirMapset + "/audio.mp3", QByteArray(1000, 'x')));
    const auto theirs = trackMapset(theirMapset, &err);
    QVERIFY(theirs.has_value());
    QTest::qWait(15);
    QVERIFY(writeFile(theirMapset + "/audio.mp3", QByteArray(1000, 'y')));       // updated
    QVERIFY(writeFile(theirMapset + "/soft-hitclap.wav", QByteArray(500, 'z'))); // added
    QVERIFY(snapshotMapset(*theirs, "manual", {}, &err).has_value());
    const QString bundle = theirSongs.path() + "/theirs.ovcz";
    QVERIFY(exportBundle(*theirs, bundle, false, &err));

    // Ours left its media alone → both come across, no conflict.
    const auto outcome = collabMergeBundle(*ours, bundle, &err);
    QVERIFY2(outcome.has_value(), qPrintable(err));
    QVERIFY(outcome->report.anyChange());
    QCOMPARE(outcome->report.mediaWritten(), 2);
    QCOMPARE(outcome->report.mediaKeptOurs(), 0);
    QCOMPARE(readFile(mapset + "/audio.mp3"), QByteArray(1000, 'y'));       // theirs' audio
    QCOMPARE(readFile(mapset + "/soft-hitclap.wav"), QByteArray(500, 'z')); // new sample
    auto repo = ShadowRepo::open(ours->repoDir());
    QVERIFY(repo->log()[0].subject.contains("media")); // subject notes the sync

    // Binary conflict: a fresh fork where BOTH sides change the audio → keep ours.
    // (ours' change is live-on-disk and unsnapshotted, yet still detected.)
    QTemporaryDir s2, ts2;
    const QString ms = s2.path() + "/collab", tms = ts2.path() + "/collab";
    QVERIFY(writeFile(ms + "/T - A (C) [VA].osu", osuFixture()));
    QVERIFY(writeFile(ms + "/audio.mp3", QByteArray(1000, 'x')));
    const auto o2 = trackMapset(ms, &err);
    QVERIFY(o2.has_value());
    QVERIFY(writeFile(tms + "/T - A (C) [VA].osu", osuFixture()));
    QVERIFY(writeFile(tms + "/audio.mp3", QByteArray(1000, 'x')));
    const auto t2 = trackMapset(tms, &err);
    QVERIFY(t2.has_value());
    QTest::qWait(15);
    QVERIFY(writeFile(tms + "/audio.mp3", QByteArray(1000, 'y'))); // theirs' audio
    QVERIFY(snapshotMapset(*t2, "manual", {}, &err).has_value());
    const QString b2 = ts2.path() + "/theirs.ovcz";
    QVERIFY(exportBundle(*t2, b2, false, &err));
    QVERIFY(writeFile(ms + "/audio.mp3", QByteArray(1000, 'w'))); // ours diverges too

    const auto oc = collabMergeBundle(*o2, b2, &err);
    QVERIFY2(oc.has_value(), qPrintable(err));
    QCOMPARE(oc->report.mediaWritten(), 0);
    QCOMPARE(oc->report.mediaKeptOurs(), 1);
    QCOMPARE(readFile(ms + "/audio.mp3"), QByteArray(1000, 'w')); // ours kept, not clobbered
}

QTEST_GUILESS_MAIN(TestBundle)
#include "test_bundle.moc"
