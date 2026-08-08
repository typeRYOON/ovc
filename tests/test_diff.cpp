#include <osu/canonical.h>
#include <osu/diff.h>
#include <osu/parser.h>
#include <QDirIterator>
#include <QFile>
#include <QtTest>

using namespace ovc::osu;

namespace {

// 7K mania fixture. Columns at CS7: x36→0, x256→3, x402→5, x475→6.
const QByteArray kBase =
    "osu file format v14\n"
    "\n"
    "[General]\n"
    "AudioFilename: audio.mp3\n"
    "Mode: 3\n"
    "\n"
    "[Editor]\n"
    "Bookmarks: 1000,2000\n"
    "BeatDivisor: 4\n"
    "\n"
    "[Metadata]\n"
    "Title:T\n"
    "Artist:A\n"
    "Creator:C\n"
    "Version:V\n"
    "Tags:one two\n"
    "BeatmapID:1\n"
    "BeatmapSetID:2\n"
    "\n"
    "[Difficulty]\n"
    "HPDrainRate:8\n"
    "CircleSize:7\n"
    "OverallDifficulty:8\n"
    "ApproachRate:5\n"
    "SliderMultiplier:1.4\n"
    "SliderTickRate:1\n"
    "\n"
    "[Events]\n"
    "//Background and Video events\n"
    "0,0,\"bg.jpg\",0,0\n"
    "//Break Periods\n"
    "2,5000,6000\n"
    "//Storyboard Layer 0 (Background)\n"
    "\n"
    "[TimingPoints]\n"
    "1000,315.789473684211,4,2,1,70,1,0\n"
    "2000,-100,4,2,1,70,0,1\n"
    "\n"
    "[HitObjects]\n"
    "36,192,1000,1,0,0:0:0:0:\n"
    "256,192,1000,5,2,0:0:0:0:\n"
    "402,192,1500,128,0,2000:0:0:0:70:snare.wav\n";

CanonicalMap canon(const QByteArray& text)
{
    return canonicalize(parseOsu(text).doc);
}

BeatmapDiff diffTexts(const QByteArray& a, const QByteArray& b)
{
    return diffBeatmaps(canon(a), canon(b));
}

QByteArray edited(const QByteArray& from, const QByteArray& to)
{
    QByteArray copy = kBase;
    copy.replace(from, to);
    return copy;
}

QString corpusFile(const QString& needle)
{
    QDirIterator it(QStringLiteral(OVC_CORPUS_DIR), {"*.osu"}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString f = it.next();
        if (f.contains(needle)) return f;
    }
    return {};
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class TestDiff : public QObject {
    Q_OBJECT

private slots:
    void identityEmpty();
    void corpusSelfDiffEmpty();
    void chordReorderEmpty();
    void numericEquivalentEmpty();
    void comboBitNoiseIgnored();
    void noteAddRemove();
    void holdExtend();
    void hitsoundChange();
    void sampleFilenameChange();
    void svChange();
    void bpmChange();
    void timingAdd();
    void breakChanges();
    void bookmarksAndTags();
    void metadataChange();
    void columnMove();
    void csChange();
    void modeChange();
    void duplicateKeysDeterministic();
    void hitsoundHolderReal();
    void affectedRange();
};

void TestDiff::identityEmpty()
{
    QVERIFY(diffTexts(kBase, kBase).isEmpty());
}

void TestDiff::corpusSelfDiffEmpty()
{
    QDirIterator it(QStringLiteral(OVC_CORPUS_DIR), {"*.osu"}, QDir::Files,
                    QDirIterator::Subdirectories);
    int checked = 0;
    while (it.hasNext()) {
        const QByteArray bytes = readFile(it.next());
        QVERIFY(diffTexts(bytes, bytes).isEmpty());
        ++checked;
    }
    QVERIFY(checked > 0);
}

void TestDiff::chordReorderEmpty()
{
    const QByteArray reordered = edited(
        "36,192,1000,1,0,0:0:0:0:\n256,192,1000,5,2,0:0:0:0:\n",
        "256,192,1000,5,2,0:0:0:0:\n36,192,1000,1,0,0:0:0:0:\n");
    QVERIFY(kBase != reordered);
    QVERIFY(diffTexts(kBase, reordered).isEmpty());
}

void TestDiff::numericEquivalentEmpty()
{
    QByteArray b = kBase;
    b.replace("2000,-100,4,2,1,70,0,1", "2000,-100.0,4,2,1,70.0,0,1");
    QVERIFY(diffTexts(kBase, b).isEmpty());
}

void TestDiff::comboBitNoiseIgnored()
{
    // type 5 = 1|4 (new combo) — meaningless in mania, must not diff.
    const QByteArray b = edited("256,192,1000,5,2,", "256,192,1000,1,2,");
    QVERIFY(diffTexts(kBase, b).isEmpty());
}

void TestDiff::noteAddRemove()
{
    const QByteArray added = kBase + "475,192,1750,1,0,0:0:0:0:\n";
    BeatmapDiff d = diffTexts(kBase, added);
    QCOMPARE(d.notes.size(), 1);
    QCOMPARE(d.notes[0].op, ChangeOp::Added);
    QCOMPARE(d.notes[0].timeMs, 1750);
    QCOMPARE(d.notes[0].column, 6);
    QVERIFY(d.summary().contains("+1"));

    d = diffTexts(added, kBase);
    QCOMPARE(d.notes.size(), 1);
    QCOMPARE(d.notes[0].op, ChangeOp::Removed);
}

void TestDiff::holdExtend()
{
    const QByteArray b = edited("128,0,2000:0:0:0:70:", "128,0,2100:0:0:0:70:");
    const BeatmapDiff d = diffTexts(kBase, b);
    QCOMPARE(d.notes.size(), 1);
    QCOMPARE(d.notes[0].op, ChangeOp::Modified);
    QCOMPARE(d.notes[0].timeMs, 1500);
    QCOMPARE(d.notes[0].column, 5);
    QCOMPARE(d.notes[0].fields.size(), 1);
    QCOMPARE(d.notes[0].fields[0].key, QByteArrayLiteral("endTime"));
}

void TestDiff::hitsoundChange()
{
    const QByteArray b = edited("256,192,1000,5,2,", "256,192,1000,5,8,");
    const BeatmapDiff d = diffTexts(kBase, b);
    QCOMPARE(d.notes.size(), 1);
    QCOMPARE(d.notes[0].fields.size(), 1);
    QCOMPARE(d.notes[0].fields[0].key, QByteArrayLiteral("hitSound"));
}

void TestDiff::sampleFilenameChange()
{
    const QByteArray b = edited("snare.wav", "kick.wav");
    const BeatmapDiff d = diffTexts(kBase, b);
    QCOMPARE(d.notes.size(), 1);
    QCOMPARE(d.notes[0].fields.size(), 1);
    QCOMPARE(d.notes[0].fields[0].key, QByteArrayLiteral("samples"));
}

void TestDiff::svChange()
{
    const QByteArray b = edited("2000,-100,", "2000,-83.3333333333333,");
    const BeatmapDiff d = diffTexts(kBase, b);
    QCOMPARE(d.timing.size(), 1);
    QCOMPARE(d.timing[0].op, ChangeOp::Modified);
    QVERIFY(!d.timing[0].uninherited);
    QCOMPARE(d.timing[0].fields.size(), 1);
    QCOMPARE(d.timing[0].fields[0].key, QByteArrayLiteral("beatLength"));
    QVERIFY(d.summary().contains("SV"));
}

void TestDiff::bpmChange()
{
    const QByteArray b = edited("1000,315.789473684211,", "1000,300,");
    const BeatmapDiff d = diffTexts(kBase, b);
    QCOMPARE(d.timing.size(), 1);
    QVERIFY(d.timing[0].uninherited);
    QVERIFY(d.summary().contains("BPM"));
}

void TestDiff::timingAdd()
{
    const QByteArray b =
        edited("2000,-100,4,2,1,70,0,1\n", "1500,-50,4,2,1,70,0,0\n2000,-100,4,2,1,70,0,1\n");
    const BeatmapDiff d = diffTexts(kBase, b);
    QCOMPARE(d.timing.size(), 1);
    QCOMPARE(d.timing[0].op, ChangeOp::Added);
    QCOMPARE(d.timing[0].timeQ, qint64(1500000));
}

void TestDiff::breakChanges()
{
    BeatmapDiff d = diffTexts(kBase, edited("2,5000,6000", "2,5000,6500"));
    QCOMPARE(d.events.breaks.size(), 1);
    QCOMPARE(d.events.breaks[0].op, ChangeOp::Modified);

    d = diffTexts(kBase, edited("2,5000,6000\n", "2,5000,6000\n2,7000,8000\n"));
    QCOMPARE(d.events.breaks.size(), 1);
    QCOMPARE(d.events.breaks[0].op, ChangeOp::Added);
}

void TestDiff::bookmarksAndTags()
{
    BeatmapDiff d = diffTexts(kBase, edited("Bookmarks: 1000,2000", "Bookmarks: 1000,2000,3000"));
    QCOMPARE(d.bookmarks.added.size(), 1);
    QCOMPARE(d.bookmarks.added[0], QByteArrayLiteral("3000"));
    QVERIFY(d.bookmarks.removed.isEmpty());

    d = diffTexts(kBase, edited("Tags:one two", "Tags:one three"));
    QCOMPARE(d.tags.added, QList<QByteArray>{QByteArrayLiteral("three")});
    QCOMPARE(d.tags.removed, QList<QByteArray>{QByteArrayLiteral("two")});
}

void TestDiff::metadataChange()
{
    const BeatmapDiff d = diffTexts(kBase, edited("Version:V\n", "Version:V2\n"));
    QCOMPARE(d.kv.size(), 1);
    QCOMPARE(d.kv[0].section, SectionId::Metadata);
    QCOMPARE(d.kv[0].changes.size(), 1);
    QCOMPARE(d.version, QStringLiteral("V2"));
}

void TestDiff::columnMove()
{
    // 402 (col 5) → 329 (col 4), same time/payload: a move, not add+remove.
    const QByteArray b = edited("402,192,1500,128,0,2000:", "329,192,1500,128,0,2000:");
    const BeatmapDiff d = diffTexts(kBase, b);
    int visible = 0;
    const NoteChange* moved = nullptr;
    for (const NoteChange& n : d.notes) {
        if (n.moveSuppressed) continue;
        ++visible;
        if (n.movedFromColumn >= 0) moved = &n;
    }
    QCOMPARE(visible, 1);
    QVERIFY(moved);
    QCOMPARE(moved->movedFromColumn, 5);
    QCOMPARE(moved->column, 4);
    QVERIFY(d.summary().contains("moved"));
}

void TestDiff::csChange()
{
    const BeatmapDiff d = diffTexts(kBase, edited("CircleSize:7", "CircleSize:8"));
    QVERIFY(d.keyCountChanged);
    QCOMPARE(d.keyCountBefore, 7);
    QCOMPARE(d.keyCountAfter, 8);
    QVERIFY(d.notes.isEmpty());
    QVERIFY(!d.isEmpty());
}

void TestDiff::modeChange()
{
    const BeatmapDiff d = diffTexts(kBase, edited("Mode: 3", "Mode: 1"));
    QVERIFY(d.modeChanged);
    QVERIFY(d.notes.isEmpty());
}

void TestDiff::duplicateKeysDeterministic()
{
    const QByteArray dup = kBase + "36,192,1000,1,0,0:0:0:0:\n";
    const BeatmapDiff d = diffTexts(kBase, dup);
    QCOMPARE(d.notes.size(), 1);
    QCOMPARE(d.notes[0].op, ChangeOp::Added);
    QCOMPARE(d.notes[0].timeMs, 1000);
    // Same input twice → same result (occurrence keying is stable).
    const BeatmapDiff d2 = diffTexts(kBase, dup);
    QCOMPARE(d2.notes.size(), 1);
}

void TestDiff::hitsoundHolderReal()
{
    const QString hs = corpusFile("Hitsounds");
    const QString lb = corpusFile("Lagrange Blossom");
    QVERIFY(!hs.isEmpty() && !lb.isEmpty());
    const BeatmapDiff d = diffTexts(readFile(hs), readFile(lb));
    QVERIFY(!d.isEmpty());
    QVERIFY(!d.modeChanged);
    QVERIFY(!d.keyCountChanged); // both 7K
    QVERIFY(d.notes.size() > 1000); // wildly different charts
    QVERIFY(d.version == QStringLiteral("Lagrange Blossom"));
    const auto range = d.affectedTimeRange();
    QVERIFY(range.first >= 0 && range.second > range.first);
}

void TestDiff::affectedRange()
{
    const QByteArray b = edited("128,0,2000:0:0:0:70:", "128,0,2100:0:0:0:70:");
    const auto range = diffTexts(kBase, b).affectedTimeRange();
    QCOMPARE(range.first, 1500);
    QCOMPARE(range.second, 2100);
}

QTEST_GUILESS_MAIN(TestDiff)
#include "test_diff.moc"
