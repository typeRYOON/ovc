#include <osu/parser.h>
#include <osu/peek.h>
#include <osu/serializer.h>
#include <osu/token.h>
#include <QDirIterator>
#include <QFile>
#include <QRandomGenerator>
#include <QtTest>

using namespace ovc::osu;

namespace {

QByteArray readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

QStringList osuFilesUnder(const QString& dir)
{
    QStringList files;
    QDirIterator it(dir, {"*.osu"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) files << it.next();
    files.sort();
    return files;
}

bool roundTrips(const QByteArray& bytes)
{
    return serializeOsu(parseOsu(bytes).doc) == bytes;
}

// Deterministic mutation: same file + index → same mutant, so failures replay.
QByteArray mutate(const QByteArray& src, quint32 seed)
{
    QRandomGenerator gen(seed);
    QByteArray m = src;
    const int ops = 1 + static_cast<int>(gen.bounded(3));
    for (int i = 0; i < ops; ++i) {
        if (m.isEmpty()) {
            m.append("x");
            continue;
        }
        const qsizetype at = gen.bounded(static_cast<quint32>(m.size()));
        switch (gen.bounded(8u)) {
        case 0: m[at] = static_cast<char>(gen.bounded(256u)); break;
        case 1: m.remove(at, 1 + gen.bounded(50u)); break;
        case 2: {
            static const char pool[] = "abc:,[]0128 \t\r\n\0//";
            QByteArray ins;
            const int n = 1 + static_cast<int>(gen.bounded(20u));
            for (int k = 0; k < n; ++k) ins += pool[gen.bounded(quint32(sizeof(pool) - 1))];
            m.insert(at, ins);
            break;
        }
        case 3: m.insert(at, m.mid(at, qMin<qsizetype>(64, m.size() - at))); break;
        case 4: m.truncate(at); break;
        case 5: m.prepend("\xEF\xBB\xBF"); break;
        case 6: m.insert(at, "\r\n[Garbage]\r\n"); break;
        case 7: m.insert(at, QByteArray(10000, 'x')); break;
        }
    }
    return m;
}

} // namespace

class TestRoundtrip : public QObject {
    Q_OBJECT

private slots:
    void corpusByteIdentical();
    void corpusStructure();
    void syntheticEdges();
    void mutationFuzz();
    void songsLibrary(); // full local library when OVC_SONGS_DIR is set
    void tokenNumEquals();
    void peekHeader();
};

void TestRoundtrip::corpusByteIdentical()
{
    const QStringList files = osuFilesUnder(QStringLiteral(OVC_CORPUS_DIR));
    QVERIFY2(!files.isEmpty(), "corpus dir has no .osu files");
    for (const QString& path : files) {
        const QByteArray bytes = readFile(path);
        QVERIFY2(roundTrips(bytes), qPrintable("round-trip failed: " + path));
    }
}

void TestRoundtrip::corpusStructure()
{
    // The playable try_unite difficulty: known layout from the plan research.
    const QStringList files = osuFilesUnder(QStringLiteral(OVC_CORPUS_DIR));
    QString lagrange;
    for (const QString& f : files)
        if (f.contains("Lagrange Blossom")) lagrange = f;
    QVERIFY(!lagrange.isEmpty());

    const auto res = parseOsu(readFile(lagrange));
    QVERIFY(res.looksLikeOsu);
    QVERIFY(res.warnings.isEmpty());
    QCOMPARE(res.doc.formatVersion, 14);
    QVERIFY(!res.doc.hadBom);

    QCOMPARE(res.doc.sections.size(), 7);
    const SectionId expected[] = {SectionId::General,    SectionId::Editor,
                                  SectionId::Metadata,   SectionId::Difficulty,
                                  SectionId::Events,     SectionId::TimingPoints,
                                  SectionId::HitObjects};
    for (int i = 0; i < 7; ++i) QCOMPARE(res.doc.sections[i].id, expected[i]);

    // Data lines: 13 timing points, 3800 hit objects (blanks excluded).
    auto dataCount = [&](SectionId id) {
        for (const Section& s : res.doc.sections)
            if (s.id == id) {
                int n = 0;
                for (const RawLine& l : s.lines)
                    if (l.kind == LineKind::Data) ++n;
                return n;
            }
        return -1;
    };
    QCOMPARE(dataCount(SectionId::TimingPoints), 13);
    QCOMPARE(dataCount(SectionId::HitObjects), 3800);
}

void TestRoundtrip::syntheticEdges()
{
    const QByteArray cases[] = {
        QByteArrayLiteral(""),
        QByteArrayLiteral("\n"),
        QByteArrayLiteral("\r\n"),
        QByteArrayLiteral("\xEF\xBB\xBF"),
        QByteArrayLiteral("\xEF\xBB\xBFosu file format v14\r\n[General]\r\nMode: 3\r\n"),
        QByteArrayLiteral("osu file format v128\n[General]\nMode: 3\n"), // lazer, LF-only
        QByteArrayLiteral("no format line at all"),
        QByteArrayLiteral("osu file format v14\r\n[HitObjects]\r\n36,192,1317,5,6,0:0:0:0:"),
        QByteArrayLiteral("lone\rcarriage\rreturns"),
        QByteArrayLiteral("[Unknown Section]\r\ndata\r\n[General]\r\nA:B\r\n[General]\r\nC:D\r\n"),
    };
    for (const QByteArray& c : cases)
        QVERIFY2(roundTrips(c), qPrintable("edge case failed: " + QString::fromUtf8(c.left(40))));

    // Classification spot checks.
    const auto res = parseOsu(cases[7]);
    QCOMPARE(res.doc.formatVersion, 14);
    QCOMPARE(res.doc.sections.size(), 1);
    QCOMPARE(res.doc.sections[0].id, SectionId::HitObjects);
    // ':' inside hitobject extras must not make it a KeyValue line.
    QCOMPARE(res.doc.sections[0].lines[0].kind, LineKind::Data);
    QCOMPARE(res.doc.sections[0].lines[0].eol, Eol::None);

    const auto dup = parseOsu(cases[9]);
    QCOMPARE(dup.doc.sections.size(), 3);
    QVERIFY(dup.warnings.size() >= 2); // unknown section + duplicate + no format line
}

void TestRoundtrip::mutationFuzz()
{
    const QStringList files = osuFilesUnder(QStringLiteral(OVC_CORPUS_DIR));
    QVERIFY(!files.isEmpty());
    for (const QString& path : files) {
        const QByteArray src = readFile(path);
        const quint32 base = qHash(path.section('/', -1));
        for (quint32 i = 0; i < 200; ++i) {
            const QByteArray m = mutate(src, base + i);
            QVERIFY2(roundTrips(m),
                     qPrintable(QStringLiteral("fuzz mutant %1 of %2 broke round-trip")
                                    .arg(i)
                                    .arg(path)));
        }
    }
}

void TestRoundtrip::songsLibrary()
{
    const QString dir = qEnvironmentVariable("OVC_SONGS_DIR");
    if (dir.isEmpty()) QSKIP("OVC_SONGS_DIR not set");
    const QStringList files = osuFilesUnder(dir);
    QVERIFY(!files.isEmpty());
    int checked = 0;
    for (const QString& path : files) {
        const QByteArray bytes = readFile(path);
        if (bytes.isEmpty()) continue;
        QVERIFY2(roundTrips(bytes), qPrintable("round-trip failed: " + path));
        ++checked;
    }
    qInfo("songs library: %d files round-tripped", checked);
}

void TestRoundtrip::tokenNumEquals()
{
    auto tok = [](const char* s) { return Token{QByteArray(s)}; };
    QVERIFY(tok("70").numEquals(tok("70.0")));
    QVERIFY(tok("-100").numEquals(tok("-100.00")));
    QVERIFY(tok("315.789473684211").numEquals(tok("315.789473684211")));
    QVERIFY(!tok("3.8").numEquals(tok("3.799999")));
    QVERIFY(!tok("abc").numEquals(tok("abd")));
    QVERIFY(tok("abc").numEquals(tok("abc"))); // byte-equal non-numerics
    QVERIFY(!tok("1").numEquals(tok("")));
}

void TestRoundtrip::peekHeader()
{
    const QStringList files = osuFilesUnder(QStringLiteral(OVC_CORPUS_DIR));
    QString lagrange;
    for (const QString& f : files)
        if (f.contains("Lagrange Blossom")) lagrange = f;
    QVERIFY(!lagrange.isEmpty());

    const auto h = peekOsuHeader(readFile(lagrange).left(8192));
    QVERIFY(h.has_value());
    QCOMPARE(h->formatVersion, 14);
    QCOMPARE(h->mode, 3);
    QCOMPARE(h->creator, QStringLiteral("Ryoon"));
    QCOMPARE(h->version, QStringLiteral("Lagrange Blossom"));
    QCOMPARE(h->beatmapId, 5804364);
    QCOMPARE(h->beatmapSetId, 2597597);

    QVERIFY(!peekOsuHeader(QByteArrayLiteral("not a beatmap")).has_value());
}

QTEST_GUILESS_MAIN(TestRoundtrip)
#include "test_roundtrip.moc"
