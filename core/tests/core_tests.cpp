// Dependency-free test harness for ovc-core. The same vectors also run under
// WASM via core/wasm/run-vectors.mjs, so keep fixtures in this file (the
// corpus sweep is native-only — it needs the filesystem).
#include <ovccore/canonical.h>
#include <ovccore/diff.h>
#include <ovccore/json.h>
#include <ovccore/merge.h>
#include <ovccore/parser.h>
#include <ovccore/peek.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        ++g_checks;                                                                     \
        if (!(cond)) {                                                                  \
            ++g_failures;                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                               \
    } while (0)

#define CHECK_EQ(a, b)                                                                  \
    do {                                                                                \
        ++g_checks;                                                                     \
        if (!((a) == (b))) {                                                            \
            ++g_failures;                                                               \
            std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b);          \
        }                                                                               \
    } while (0)

using namespace ovc::core;

std::string readFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool roundTrips(std::string_view bytes)
{
    return serializeOsu(parseOsu(bytes).doc) == bytes;
}

const std::string kBase =
    "osu file format v14\n\n[General]\nAudioFilename: audio.mp3\nMode: 3\n\n"
    "[Editor]\nBookmarks: 1000,2000\nBeatDivisor: 4\n\n"
    "[Metadata]\nTitle:T\nArtist:A\nCreator:C\nVersion:V\nTags:one two\n"
    "BeatmapID:1\nBeatmapSetID:2\n\n"
    "[Difficulty]\nHPDrainRate:8\nCircleSize:7\nOverallDifficulty:8\nApproachRate:5\n"
    "SliderMultiplier:1.4\nSliderTickRate:1\n\n"
    "[Events]\n//Background and Video events\n0,0,\"bg.jpg\",0,0\n//Break Periods\n"
    "2,5000,6000\n//Storyboard Layer 0 (Background)\n\n"
    "[TimingPoints]\n1000,315.789473684211,4,2,1,70,1,0\n2000,-100,4,2,1,70,0,1\n\n"
    "[HitObjects]\n36,192,1000,1,0,0:0:0:0:\n256,192,1000,5,2,0:0:0:0:\n"
    "402,192,1500,128,0,2000:0:0:0:70:snare.wav\n";

CanonicalMap canon(std::string_view text)
{
    return canonicalize(parseOsu(text).doc);
}

BeatmapDiff diffTexts(std::string_view a, std::string_view b)
{
    return diffBeatmaps(canon(a), canon(b));
}

std::string edited(std::string_view from, std::string_view to)
{
    std::string copy = kBase;
    const size_t at = copy.find(from);
    if (at != std::string::npos) copy.replace(at, from.size(), to);
    return copy;
}

void testTokens()
{
    auto tok = [](const char* s) { return Token{s}; };
    CHECK(tok("70").numEquals(tok("70.0")));
    CHECK(tok("-100").numEquals(tok("-100.00")));
    CHECK(!tok("3.8").numEquals(tok("3.799999")));
    CHECK(!tok("abc").numEquals(tok("abd")));
    CHECK(tok("abc").numEquals(tok("abc")));
    CHECK(!tok("1").numEquals(tok("")));
    bool ok = false;
    CHECK_EQ(tok("14").toInt(&ok), 14);
    CHECK(ok);
    tok("14x").toInt(&ok);
    CHECK(!ok);
}

void testRoundtripSynthetic()
{
    const std::string cases[] = {
        "",
        "\n",
        "\r\n",
        "\xEF\xBB\xBF",
        "\xEF\xBB\xBFosu file format v14\r\n[General]\r\nMode: 3\r\n",
        "osu file format v128\n[General]\nMode: 3\n",
        "no format line at all",
        "osu file format v14\r\n[HitObjects]\r\n36,192,1317,5,6,0:0:0:0:",
        "lone\rcarriage\rreturns",
        "[Unknown Section]\r\ndata\r\n[General]\r\nA:B\r\n[General]\r\nC:D\r\n",
        kBase,
    };
    for (const std::string& c : cases) CHECK(roundTrips(c));

    const auto res = parseOsu("osu file format v14\r\n[HitObjects]\r\n36,192,1317,5,6,0:0:0:0:");
    CHECK_EQ(res.doc.formatVersion, 14);
    CHECK_EQ(res.doc.sections.size(), size_t(1));
    CHECK(res.doc.sections[0].id == SectionId::HitObjects);
    CHECK(res.doc.sections[0].lines[0].kind == LineKind::Data);
    CHECK(res.doc.sections[0].lines[0].eol == Eol::None);
}

void testRoundtripFuzz()
{
    std::mt19937 gen(0xC0FFEE);
    auto rnd = [&](size_t n) { return gen() % n; };
    for (int i = 0; i < 300; ++i) {
        std::string m = kBase;
        const int ops = 1 + int(rnd(3));
        for (int k = 0; k < ops; ++k) {
            if (m.empty()) m = "x";
            const size_t at = rnd(m.size());
            switch (rnd(8)) {
            case 0: m[at] = char(rnd(256)); break;
            case 1: m.erase(at, 1 + rnd(50)); break;
            case 2: m.insert(at, "ab:,[]\r\n\t x"); break;
            case 3: m.insert(at, m.substr(at, std::min<size_t>(64, m.size() - at))); break;
            case 4: m.resize(at); break;
            case 5: m.insert(0, "\xEF\xBB\xBF"); break;
            case 6: m.insert(at, "\r\n[Garbage]\r\n"); break;
            case 7: m.insert(at, std::string(5000, 'x')); break;
            }
        }
        CHECK(roundTrips(m));
    }
}

void testPeek()
{
    const auto h = peekOsuHeader(kBase);
    CHECK(h.has_value());
    CHECK_EQ(h->formatVersion, 14);
    CHECK_EQ(h->mode, 3);
    CHECK_EQ(h->creator, std::string("C"));
    CHECK_EQ(h->version, std::string("V"));
    CHECK_EQ(h->beatmapId, 1);
    CHECK_EQ(h->beatmapSetId, 2);
    CHECK_EQ(h->title, std::string("T"));  // no *Unicode -> ascii Title
    CHECK_EQ(h->artist, std::string("A"));
    CHECK(!peekOsuHeader("not a beatmap").has_value());

    // Native (unicode) title/artist override ascii when present; empty falls back.
    const auto u = peekOsuHeader(
        "osu file format v14\n\n[Metadata]\n"
        "Title:Romaji\nTitleUnicode:\xe9\x80\x80\xe5\xb1\x88\n"
        "Artist:Ascii\nArtistUnicode:\n");
    CHECK(u.has_value());
    CHECK_EQ(u->title, std::string("\xe9\x80\x80\xe5\xb1\x88")); // TitleUnicode wins
    CHECK_EQ(u->artist, std::string("Ascii"));                  // empty ArtistUnicode -> ascii
}

void testDiffBasics()
{
    CHECK(diffTexts(kBase, kBase).empty());

    // Chord reorder: canonicalization kills phantom diffs.
    const std::string reordered = edited("36,192,1000,1,0,0:0:0:0:\n256,192,1000,5,2,0:0:0:0:\n",
                                         "256,192,1000,5,2,0:0:0:0:\n36,192,1000,1,0,0:0:0:0:\n");
    CHECK(kBase != reordered);
    CHECK(diffTexts(kBase, reordered).empty());

    // Numeric-equivalent rewrite.
    CHECK(diffTexts(kBase, edited("2000,-100,4,2,1,70,0,1", "2000,-100.0,4,2,1,70.0,0,1"))
              .empty());

    // Mania combo bit is noise.
    CHECK(diffTexts(kBase, edited("256,192,1000,5,2,", "256,192,1000,1,2,")).empty());

    // Note add.
    BeatmapDiff d = diffTexts(kBase, kBase + "475,192,1750,1,0,0:0:0:0:\n");
    CHECK_EQ(d.notes.size(), size_t(1));
    CHECK(d.notes[0].op == ChangeOp::Added);
    CHECK_EQ(d.notes[0].timeMs, 1750);
    CHECK_EQ(d.notes[0].column, 6);
    CHECK(d.summary().find("+1") != std::string::npos);

    // Hold extend → endTime field.
    d = diffTexts(kBase, edited("128,0,2000:0:0:0:70:", "128,0,2100:0:0:0:70:"));
    CHECK_EQ(d.notes.size(), size_t(1));
    CHECK_EQ(d.notes[0].fields.size(), size_t(1));
    CHECK_EQ(d.notes[0].fields[0].key, std::string("endTime"));
    CHECK_EQ(d.affectedTimeRange().first, 1500);
    CHECK_EQ(d.affectedTimeRange().second, 2100);

    // Hitsound + sample changes.
    d = diffTexts(kBase, edited("256,192,1000,5,2,", "256,192,1000,5,8,"));
    CHECK_EQ(d.notes[0].fields[0].key, std::string("hitSound"));
    d = diffTexts(kBase, edited("snare.wav", "kick.wav"));
    CHECK_EQ(d.notes[0].fields[0].key, std::string("samples"));

    // SV vs BPM labeling.
    d = diffTexts(kBase, edited("2000,-100,", "2000,-83.3333333333333,"));
    CHECK_EQ(d.timing.size(), size_t(1));
    CHECK(!d.timing[0].uninherited);
    CHECK(d.summary().find("SV") != std::string::npos);
    d = diffTexts(kBase, edited("1000,315.789473684211,", "1000,300,"));
    CHECK(d.timing[0].uninherited);
    CHECK(d.summary().find("BPM") != std::string::npos);

    // Column move pairing.
    d = diffTexts(kBase, edited("402,192,1500,128,0,2000:", "329,192,1500,128,0,2000:"));
    int visible = 0;
    const NoteChange* moved = nullptr;
    for (const NoteChange& n : d.notes) {
        if (n.moveSuppressed) continue;
        ++visible;
        if (n.movedFromColumn >= 0) moved = &n;
    }
    CHECK_EQ(visible, 1);
    CHECK(moved && moved->movedFromColumn == 5 && moved->column == 4);

    // CS / mode bailouts.
    d = diffTexts(kBase, edited("CircleSize:7", "CircleSize:8"));
    CHECK(d.keyCountChanged);
    CHECK(d.notes.empty());
    CHECK(!d.empty());
    d = diffTexts(kBase, edited("Mode: 3", "Mode: 1"));
    CHECK(d.modeChanged);

    // Breaks / bookmarks / tags.
    d = diffTexts(kBase, edited("2,5000,6000", "2,5000,6500"));
    CHECK_EQ(d.events.breaks.size(), size_t(1));
    d = diffTexts(kBase, edited("Bookmarks: 1000,2000", "Bookmarks: 1000,2000,3000"));
    CHECK_EQ(d.bookmarks.added.size(), size_t(1));
    CHECK_EQ(d.bookmarks.added[0], std::string("3000"));
    d = diffTexts(kBase, edited("Tags:one two", "Tags:one three"));
    CHECK_EQ(d.tags.added.size(), size_t(1));
    CHECK_EQ(d.tags.removed.size(), size_t(1));

    // Duplicate keys stay deterministic.
    d = diffTexts(kBase, kBase + "36,192,1000,1,0,0:0:0:0:\n");
    CHECK_EQ(d.notes.size(), size_t(1));
    CHECK(d.notes[0].op == ChangeOp::Added);
}

void testJson()
{
    const BeatmapDiff d = diffTexts(kBase, kBase + "475,192,1750,1,0,0:0:0:0:\n");
    const std::string js = diffToJson(d);
    CHECK(js.find("\"op\":\"added\"") != std::string::npos);
    CHECK(js.find("\"timeMs\":1750") != std::string::npos);
    CHECK(js.find("\"summary\"") != std::string::npos);
    CHECK(js.find("\"empty\":false") != std::string::npos);

    // Timing change carries milli-fixed-point BPM/SV (no float drift).
    // beatLength 300 ms/beat → 200 BPM → 200000 milli-BPM.
    const std::string tj =
        diffToJson(diffTexts(kBase, edited("1000,315.789473684211,", "1000,300,")));
    CHECK(tj.find("\"bpmMilli\":200000") != std::string::npos);
    CHECK(tj.find("\"svMilli\":1000") != std::string::npos);

    const std::string mj = mapToJson(canon(kBase));
    CHECK(mj.find("\"keyCount\":7") != std::string::npos);
    CHECK(mj.find("\"audioFilename\":\"audio.mp3\"") != std::string::npos);
    CHECK(mj.find("[1500,5,2000]") != std::string::npos); // the hold note triple
}

void testCorpus(const char* dir)
{
    namespace fs = std::filesystem;
    int files = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file() || entry.path().extension() != ".osu") continue;
        const std::string bytes = readFile(entry.path());
        CHECK(roundTrips(bytes));
        const CanonicalMap m = canonicalize(parseOsu(bytes).doc);
        CHECK(diffBeatmaps(m, m).empty());
        // emit(m) must re-canonicalize to the same semantic content.
        CHECK(diffBeatmaps(m, canonicalize(parseOsu(emitCanonical(m)).doc)).empty());
        ++files;
    }
    CHECK(files > 0);
    std::printf("corpus: %d files round-tripped + self-diffed\n", files);
}

// emit(m) must canonicalize back to the same semantic map.
bool emitRoundTrips(std::string_view osu)
{
    const CanonicalMap a = canon(osu);
    const CanonicalMap b = canon(emitCanonical(a));
    // Compare the semantic content via the diff engine — an empty diff means
    // the two maps are semantically identical.
    return diffBeatmaps(a, b).empty() && a.keyCount == b.keyCount && a.mode == b.mode;
}

void testEmit()
{
    CHECK(emitRoundTrips(kBase));
    // With holds, custom samples, kiai, multiple sections.
    CHECK(emitRoundTrips(edited("128,0,2000:0:0:0:70:snare.wav", "128,0,2100:0:0:0:70:kick.wav")));
    // Corpus files (native only) also survive the emit round-trip in testCorpus.
}

CanonicalMap canonOf(const std::string& s) { return canonicalize(parseOsu(s).doc); }

void testMerge()
{
    // Two disjoint edits auto-merge: ours adds a note, theirs changes OD.
    const std::string base = kBase;
    std::string ours = base;
    ours.insert(ours.find("402,192,1500,"), "475,192,1750,1,0,0:0:0:0:\n"); // add col6 note
    std::string theirs = base;
    theirs.replace(theirs.find("OverallDifficulty:8"), 19, "OverallDifficulty:6");

    MergeResult m = merge3(canonOf(base), canonOf(ours), canonOf(theirs));
    CHECK(m.clean());
    // The merged map has BOTH changes: the new note and OD 6.
    const CanonicalMap& mm = m.merged;
    CHECK(mm.kv(SectionId::Difficulty, "OverallDifficulty")->raw == "6");
    bool hasAddedNote = false;
    for (const CanonicalNote& n : mm.notes)
        if (n.timeMs == 1750 && n.column == 6) hasAddedNote = true;
    CHECK(hasAddedNote);
    // And it emits to a valid file that re-diffs clean against itself.
    CHECK(diffBeatmaps(mm, canonOf(emitCanonical(mm))).empty());

    // Same field, both changed differently → conflict; ours wins the output.
    std::string oursOD = base, theirsOD = base;
    oursOD.replace(oursOD.find("OverallDifficulty:8"), 19, "OverallDifficulty:5");
    theirsOD.replace(theirsOD.find("OverallDifficulty:8"), 19, "OverallDifficulty:9");
    MergeResult c = merge3(canonOf(base), canonOf(oursOD), canonOf(theirsOD));
    CHECK(!c.clean());
    CHECK(c.conflicts.size() == 1);
    CHECK(c.conflicts[0].domain == MergeDomain::Kv);
    CHECK(c.conflicts[0].key == "OverallDifficulty");
    CHECK(c.conflicts[0].ours == "5" && c.conflicts[0].theirs == "9");
    CHECK(c.merged.kv(SectionId::Difficulty, "OverallDifficulty")->raw == "5"); // ours

    // Same change on both sides is not a conflict.
    MergeResult s = merge3(canonOf(base), canonOf(oursOD), canonOf(oursOD));
    CHECK(s.clean());

    // Disjoint note edits on different columns auto-merge; same note edited two
    // ways conflicts.
    std::string oursNote = base, theirsNote = base;
    oursNote.replace(oursNote.find("36,192,1000,1,0,"), 16, "36,192,1000,1,8,"); // ours: hitsound
    theirsNote.replace(theirsNote.find("256,192,1000,5,2,"), 17,
                       "256,192,1000,5,8,"); // theirs: different note's hitsound
    MergeResult dn = merge3(canonOf(base), canonOf(oursNote), canonOf(theirsNote));
    CHECK(dn.clean()); // different notes → clean

    std::string oursSame = base, theirsSame = base;
    oursSame.replace(oursSame.find("36,192,1000,1,0,"), 16, "36,192,1000,1,8,");
    theirsSame.replace(theirsSame.find("36,192,1000,1,0,"), 16, "36,192,1000,1,4,");
    MergeResult sn = merge3(canonOf(base), canonOf(oursSame), canonOf(theirsSame));
    CHECK(!sn.clean());
    CHECK(sn.conflicts[0].domain == MergeDomain::Notes);
    CHECK(sn.conflicts[0].column == 0);

    // Per-field within-note merge: the SAME hold, ours re-hitsounds it while
    // theirs lengthens it → different fields, so it merges clean (both edits kept).
    std::string oursHs = base, theirsEnd = base;
    oursHs.replace(oursHs.find("402,192,1500,128,0,"), 19, "402,192,1500,128,2,"); // ours: hitSound 0→2
    theirsEnd.replace(theirsEnd.find("128,0,2000:"), 11, "128,0,2100:");           // theirs: end 2000→2100
    MergeResult pf = merge3(canonOf(base), canonOf(oursHs), canonOf(theirsEnd));
    CHECK(pf.clean());
    bool sawMerged = false;
    for (const CanonicalNote& n : pf.merged.notes)
        if (n.timeMs == 1500 && n.column == 5) {
            sawMerged = true;
            CHECK(n.hitSound.raw == "2");             // ours' hitsound
            CHECK(n.isHold && n.endTimeMs == 2100);   // theirs' end time
        }
    CHECK(sawMerged);
    CHECK(diffBeatmaps(pf.merged, canonOf(emitCanonical(pf.merged))).empty()); // rebuilt note emits valid

    // Same field two ways (both move the LN end) → one shape conflict; ours wins.
    std::string oursEnd = base, theirsEnd2 = base;
    oursEnd.replace(oursEnd.find("128,0,2000:"), 11, "128,0,2100:");
    theirsEnd2.replace(theirsEnd2.find("128,0,2000:"), 11, "128,0,2300:");
    MergeResult pfc = merge3(canonOf(base), canonOf(oursEnd), canonOf(theirsEnd2));
    CHECK(!pfc.clean());
    CHECK(pfc.conflicts.size() == 1);
    CHECK(pfc.conflicts[0].domain == MergeDomain::Notes);
    CHECK(pfc.conflicts[0].id.find(":shape") != std::string::npos);
    for (const CanonicalNote& n : pfc.merged.notes)
        if (n.timeMs == 1500 && n.column == 5) CHECK(n.endTimeMs == 2100); // ours wins

    // Key-count mismatch bails whole-file.
    std::string cs = base;
    cs.replace(cs.find("CircleSize:7"), 12, "CircleSize:4");
    MergeResult wf = merge3(canonOf(base), canonOf(base), canonOf(cs));
    CHECK(wf.wholeFileConflict);
    CHECK(!wf.reason.empty());

    // Bookmarks/tags are set-merged (both additions survive, no conflict).
    std::string oursBm = base, theirsBm = base;
    oursBm.replace(oursBm.find("Bookmarks: 1000,2000"), 20, "Bookmarks: 1000,2000,3000");
    theirsBm.replace(theirsBm.find("Bookmarks: 1000,2000"), 20, "Bookmarks: 1000,2000,4000");
    MergeResult bm = merge3(canonOf(base), canonOf(oursBm), canonOf(theirsBm));
    CHECK(bm.clean());
    CHECK(bm.merged.bookmarks.size() == 4); // 1000,2000,3000,4000

    // Resolutions: re-run the OD conflict choosing theirs by conflict id.
    MergeResult cc = merge3(canonOf(base), canonOf(oursOD), canonOf(theirsOD));
    CHECK(cc.conflicts.size() == 1);
    const std::string cid = cc.conflicts[0].id;
    CHECK(cid == "kv:Difficulty:OverallDifficulty");
    ResolutionMap res{{cid, ResolveSide::Theirs}};
    MergeResult resolved = merge3(canonOf(base), canonOf(oursOD), canonOf(theirsOD), res);
    // Still reports the conflict (so a UI can show it was resolved), but the
    // merged value is now theirs (9), not ours (5).
    CHECK(resolved.merged.kv(SectionId::Difficulty, "OverallDifficulty")->raw == "9");
    // A note conflict resolves by its id too.
    MergeResult resolvedNote =
        merge3(canonOf(base), canonOf(oursSame), canonOf(theirsSame),
               {{sn.conflicts[0].id, ResolveSide::Theirs}});
    for (const CanonicalNote& n : resolvedNote.merged.notes)
        if (n.timeMs == 1000 && n.column == 0)
            CHECK(n.hitSound.raw == "4"); // theirs' value
}

// std/catch position is a diffed + merged field — moving objects is the primary
// std edit, and it was invisible before (notes key by time only).
void testStdCatchPosition()
{
    const std::string sbase =
        "osu file format v14\n\n[General]\nMode: 0\n\n"
        "[Metadata]\nTitle:T\nCreator:C\nVersion:V\n\n"
        "[Difficulty]\nCircleSize:4\nApproachRate:9\nSliderMultiplier:1.4\n\n"
        "[TimingPoints]\n0,500,4,2,0,50,1,0\n\n"
        "[HitObjects]\n100,100,1000,1,0,0:0:0:0:\n200,200,2000,1,0,0:0:0:0:\n";
    auto sEdit = [&](std::string_view from, std::string_view to) {
        std::string c = sbase; const size_t at = c.find(from);
        if (at != std::string::npos) c.replace(at, from.size(), to);
        return c;
    };

    // Move a circle in x AND y → one modified note carrying both fields.
    BeatmapDiff d = diffBeatmaps(canonOf(sbase), canonOf(sEdit("100,100,1000,1,0,", "150,120,1000,1,0,")));
    CHECK_EQ(d.notes.size(), size_t(1));
    CHECK(d.notes[0].op == ChangeOp::Modified);
    bool sawX = false, sawY = false;
    for (const auto& f : d.notes[0].fields) { sawX |= f.key == "x"; sawY |= f.key == "y"; }
    CHECK(sawX && sawY);
    CHECK(d.summary().find("~1") != std::string::npos);

    // Two mappers move DIFFERENT circles → both moves survive.
    MergeResult mm = merge3(canonOf(sbase), canonOf(sEdit("100,100,1000,1,0,", "150,150,1000,1,0,")),
                            canonOf(sEdit("200,200,2000,1,0,", "260,240,2000,1,0,")));
    CHECK(mm.clean());
    for (const CanonicalNote& n : mm.merged.notes) {
        if (n.timeMs == 1000) CHECK(n.x.raw == "150" && n.y.raw == "150"); // ours
        if (n.timeMs == 2000) CHECK(n.x.raw == "260" && n.y.raw == "240"); // theirs
    }
    CHECK(diffBeatmaps(mm.merged, canonOf(emitCanonical(mm.merged))).empty());

    // Position vs hitsound on the SAME circle merges per-field (both kept).
    MergeResult pf = merge3(canonOf(sbase), canonOf(sEdit("100,100,1000,1,0,", "150,150,1000,1,0,")),
                            canonOf(sEdit("100,100,1000,1,0,", "100,100,1000,1,8,")));
    CHECK(pf.clean());
    for (const CanonicalNote& n : pf.merged.notes)
        if (n.timeMs == 1000) { CHECK(n.x.raw == "150" && n.y.raw == "150"); CHECK(n.hitSound.raw == "8"); }

    // Same circle moved two ways → a position conflict; ours wins the output.
    MergeResult pc = merge3(canonOf(sbase), canonOf(sEdit("100,100,1000,1,0,", "150,150,1000,1,0,")),
                            canonOf(sEdit("100,100,1000,1,0,", "170,130,1000,1,0,")));
    CHECK(!pc.clean());
    CHECK(pc.conflicts.size() == 1);
    CHECK(pc.conflicts[0].domain == MergeDomain::Notes);
    CHECK(pc.conflicts[0].id.find(":position") != std::string::npos);
    for (const CanonicalNote& n : pc.merged.notes)
        if (n.timeMs == 1000) CHECK(n.x.raw == "150"); // ours

    // catch keys the lane on x only — moving x diffs, moving y alone does not.
    const std::string cbase =
        "osu file format v14\n\n[General]\nMode: 2\n\n[Metadata]\nTitle:T\nCreator:C\nVersion:V\n\n"
        "[Difficulty]\nCircleSize:4\nApproachRate:9\nSliderMultiplier:1.4\n\n"
        "[TimingPoints]\n0,500,4,2,0,50,1,0\n\n[HitObjects]\n100,100,1000,1,0,0:0:0:0:\n";
    auto cEdit = [&](std::string_view from, std::string_view to) {
        std::string c = cbase; const size_t at = c.find(from);
        if (at != std::string::npos) c.replace(at, from.size(), to);
        return c;
    };
    d = diffBeatmaps(canonOf(cbase), canonOf(cEdit("100,100,1000,", "160,100,1000,"))); // move x
    CHECK_EQ(d.notes.size(), size_t(1));
    CHECK_EQ(d.notes[0].fields[0].key, std::string("x"));
    d = diffBeatmaps(canonOf(cbase), canonOf(cEdit("100,100,1000,", "100,50,1000,"))); // move y only
    CHECK(d.notes.empty()); // catch ignores y

    // Slider tail decomposition: a reshape is a clean "curve" field, and slides/
    // length/edgeSounds/hitSample each diff on their own (a slider's endTimeMs
    // stays == its start time, so its extent lives only in slides + length).
    const std::string slbase =
        "osu file format v14\n\n[General]\nMode: 0\n\n"
        "[Metadata]\nTitle:T\nCreator:C\nVersion:V\n\n"
        "[Difficulty]\nCircleSize:4\nApproachRate:9\nSliderMultiplier:1.4\nSliderTickRate:1\n\n"
        "[TimingPoints]\n0,500,4,2,0,50,1,0\n\n"
        "[HitObjects]\n100,100,1000,2,0,B|200:100|300:100,1,200,0|0,0:0|0:0,0:0:0:0:\n";
    auto slEdit = [&](std::string_view from, std::string_view to) {
        std::string c = slbase; const size_t at = c.find(from);
        if (at != std::string::npos) c.replace(at, from.size(), to);
        return c;
    };
    auto only = [](const BeatmapDiff& dd, const char* key) {
        return dd.notes.size() == 1 && dd.notes[0].fields.size() == 1 && dd.notes[0].fields[0].key == key;
    };
    CHECK(only(diffBeatmaps(canonOf(slbase), canonOf(slEdit("B|200:100|300:100", "B|200:150|300:100"))), "curve"));
    CHECK(only(diffBeatmaps(canonOf(slbase), canonOf(slEdit(",1,200,", ",1,300,"))), "length"));
    CHECK(only(diffBeatmaps(canonOf(slbase), canonOf(slEdit(",1,200,", ",2,200,"))), "slides"));
    CHECK(only(diffBeatmaps(canonOf(slbase), canonOf(slEdit("0:0|0:0,0:0:0:0:", "0:0|0:0,2:0:0:0:"))), "samples"));
    {
        const BeatmapDiff de = diffBeatmaps(canonOf(slbase), canonOf(slEdit(",0|0,", ",0|8,")));
        bool sawEdge = false;
        for (const auto& fld : de.notes.at(0).fields) if (fld.key == "edgeSounds") sawEdge = true;
        CHECK(sawEdge);
    }
}

} // namespace

int main(int argc, char* argv[])
{
    testTokens();
    testRoundtripSynthetic();
    testRoundtripFuzz();
    testPeek();
    testDiffBasics();
    testJson();
    testEmit();
    testMerge();
    testStdCatchPosition();
#ifdef OVC_CORPUS_DIR
    testCorpus(argc > 1 ? argv[1] : OVC_CORPUS_DIR);
#endif
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
