#include <ovccore/diff.h>
#include <algorithm>
#include <climits>
#include <unordered_map>
#include <unordered_set>

namespace ovc::core {

namespace {

// Mania gameplay ignores the new-combo bit (4); stable's editor shuffles it
// freely, so comparing it would produce phantom diffs.
int comparableType(const CanonicalNote& n, int mode)
{
    return mode == 3 ? (n.typeBits & ~4) : n.typeBits;
}

std::string joinTokens(const std::vector<Token>& tokens, char sep)
{
    std::string out;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i) out += sep;
        out += tokens[i].raw;
    }
    return out;
}

bool sampleListsEqual(const std::vector<Token>& a, const std::vector<Token>& b)
{
    // Index-aligned; absent fields count as "0" (filename as ""). Filename is
    // the last populated slot and compares byte-wise.
    const size_t n = std::max(a.size(), b.size());
    const Token zero{"0"};
    for (size_t i = 0; i < n; ++i) {
        const bool isFilename = i == 4;
        const Token& ta = i < a.size() ? a[i] : (isFilename ? Token{} : zero);
        const Token& tb = i < b.size() ? b[i] : (isFilename ? Token{} : zero);
        if (isFilename ? !(ta.raw == tb.raw) : !ta.numEquals(tb)) return false;
    }
    return true;
}

bool notePayloadEquals(const CanonicalNote& a, const CanonicalNote& b, int mode)
{
    return comparableType(a, mode) == comparableType(b, mode) && a.isHold == b.isHold &&
           a.endTimeMs == b.endTimeMs && a.hitSound.numEquals(b.hitSound) &&
           sampleListsEqual(a.samplesNoEnd(), b.samplesNoEnd());
}

// A slider's tail is comma-delimited (curveType|points, slides, length,
// [edgeSounds, edgeSets,] [hitSample]) — but CanonicalNote colon-splits the whole
// tail into `samples`, so a reshape is buried in that blob. Pull the curve
// (geometry), the per-node edge hitsounds, and the real hitSample apart so each
// diffs as its own field.
struct SliderParts {
    std::string curve; // curveType|c1|c2… — the geometry
    Token slides; // repeat count (a slider's endTimeMs stays == its start, so
    Token length; // slides+length are the only record of its temporal extent)
    std::string edges; // edgeSounds[,edgeSets] — per-node hitsounds
    std::vector<Token> sample; // hitSample slots (normalSet:additionSet:index:volume:file)
};

SliderParts sliderParts(const std::string& tail)
{
    std::vector<std::string> f;
    for (size_t s = 0;;) {
        const size_t c = tail.find(',', s);
        f.push_back(tail.substr(s, c == std::string::npos ? std::string::npos : c - s));
        if (c == std::string::npos) break;
        s = c + 1;
    }
    SliderParts p;
    p.curve = f.size() > 0 ? f[0] : std::string();
    p.slides = Token{f.size() > 1 ? f[1] : std::string("1")};
    p.length = Token{f.size() > 2 ? f[2] : std::string("0")};
    // hitSample = a trailing field with a ':' (n:n:n:n[:file]); old maps omit it.
    size_t last = f.size();
    if (!f.empty() && f.back().find(':') != std::string::npos) {
        const std::string& hs = f.back();
        for (size_t s = 0;;) {
            const size_t c = hs.find(':', s);
            p.sample.push_back(Token{hs.substr(s, c == std::string::npos ? std::string::npos : c - s)});
            if (c == std::string::npos) break;
            s = c + 1;
        }
        last = f.size() - 1;
    }
    for (size_t i = 3; i < last; ++i) { // fields 3+ before the hitSample = edge sounds/sets
        if (!p.edges.empty()) p.edges += ',';
        p.edges += f[i];
    }
    return p;
}

std::vector<KvDiff> diffKvSections(const CanonicalMap& before, const CanonicalMap& after)
{
    std::vector<KvDiff> out;
    using KvList = std::vector<std::pair<std::string, Token>>;
    const std::pair<SectionId, std::pair<const KvList*, const KvList*>> sections[] = {
        {SectionId::General, {&before.general, &after.general}},
        {SectionId::Editor, {&before.editor, &after.editor}},
        {SectionId::Metadata, {&before.metadata, &after.metadata}},
        {SectionId::Difficulty, {&before.difficulty, &after.difficulty}},
    };
    for (const auto& [id, lists] : sections) {
        KvDiff d;
        d.section = id;
        std::unordered_map<std::string, const Token*> beforeMap;
        for (const auto& [k, v] : *lists.first) beforeMap.emplace(k, &v);
        std::unordered_set<std::string> seen;
        for (const auto& [k, v] : *lists.second) {
            seen.insert(k);
            const auto it = beforeMap.find(k);
            if (it == beforeMap.end())
                d.changes.push_back({k, Token{}, v});
            else if (!it->second->numEquals(v))
                d.changes.push_back({k, *it->second, v});
        }
        for (const auto& [k, v] : *lists.first)
            if (!seen.count(k)) d.changes.push_back({k, v, Token{}});
        if (!d.changes.empty()) out.push_back(std::move(d));
    }
    return out;
}

ListDiff diffSortedInts(const std::vector<int64_t>& a, const std::vector<int64_t>& b)
{
    ListDiff d;
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        if (j >= b.size() || (i < a.size() && a[i] < b[j]))
            d.removed.push_back(std::to_string(a[i++]));
        else if (i >= a.size() || b[j] < a[i])
            d.added.push_back(std::to_string(b[j++]));
        else
            ++i, ++j;
    }
    return d;
}

ListDiff diffTagSets(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    ListDiff d;
    const std::unordered_set<std::string> sa(a.begin(), a.end());
    const std::unordered_set<std::string> sb(b.begin(), b.end());
    for (const std::string& t : b)
        if (!sa.count(t)) d.added.push_back(t);
    for (const std::string& t : a)
        if (!sb.count(t)) d.removed.push_back(t);
    return d;
}

std::vector<TimingChange> diffTiming(const CanonicalMap& before, const CanonicalMap& after)
{
    static const char* kNames[] = {"time",        "beatLength",  "meter",
                                   "sampleSet",   "sampleIndex", "volume",
                                   "uninherited", "effects"};
    std::vector<TimingChange> out;
    size_t i = 0, j = 0;
    const auto& a = before.timing;
    const auto& b = after.timing;
    while (i < a.size() || j < b.size()) {
        const bool takeA = j >= b.size() || (i < a.size() && a[i].key < b[j].key);
        const bool takeB = i >= a.size() || (j < b.size() && b[j].key < a[i].key);
        if (takeA) {
            out.push_back({ChangeOp::Removed, a[i].key.timeQ, a[i].uninherited, {}, a[i], {}});
            ++i;
        }
        else if (takeB) {
            out.push_back({ChangeOp::Added, b[j].key.timeQ, b[j].uninherited, {}, {}, b[j]});
            ++j;
        }
        else {
            TimingChange c{ChangeOp::Modified, a[i].key.timeQ, a[i].uninherited, {}, a[i], b[j]};
            const int n = int(std::max(a[i].fields.size(), b[j].fields.size()));
            for (int f = 1; f < n; ++f) { // field 0 is the key itself
                if (!a[i].field(f).numEquals(b[j].field(f)))
                    c.fields.push_back(
                        {f < 8 ? kNames[f] : "extra", a[i].field(f), b[j].field(f)});
            }
            if (!c.fields.empty()) out.push_back(std::move(c));
            ++i, ++j;
        }
    }
    return out;
}

std::vector<NoteChange> diffNotes(const CanonicalMap& before, const CanonicalMap& after)
{
    std::vector<NoteChange> out;
    const int mode = after.mode;
    size_t i = 0, j = 0;
    const auto& a = before.notes;
    const auto& b = after.notes;
    while (i < a.size() || j < b.size()) {
        const bool takeA = j >= b.size() || (i < a.size() && a[i].key < b[j].key);
        const bool takeB = i >= a.size() || (j < b.size() && b[j].key < a[i].key);
        if (takeA) {
            out.push_back({ChangeOp::Removed, a[i].timeMs, a[i].column, {}, a[i], {}});
            ++i;
        }
        else if (takeB) {
            out.push_back({ChangeOp::Added, b[j].timeMs, b[j].column, {}, {}, b[j]});
            ++j;
        }
        else {
            NoteChange c{ChangeOp::Modified, a[i].timeMs, a[i].column, {}, a[i], b[j]};
            if (comparableType(a[i], mode) != comparableType(b[j], mode))
                c.fields.push_back({"type", a[i].type, b[j].type});
            if (a[i].endTimeMs != b[j].endTimeMs) {
                const Token be =
                    a[i].isHold && !a[i].samples.empty() ? a[i].samples[0] : a[i].time;
                const Token af =
                    b[j].isHold && !b[j].samples.empty() ? b[j].samples[0] : b[j].time;
                c.fields.push_back({"endTime", be, af});
            }
            if (!a[i].hitSound.numEquals(b[j].hitSound))
                c.fields.push_back({"hitSound", a[i].hitSound, b[j].hitSound});
            // Sliders: split the tail so a reshape is a clean "curve" change and
            // the real hitsample/edge-sounds diff on their own (samplesNoEnd would
            // otherwise colon-split the whole curve+metadata blob into "samples").
            if ((a[i].typeBits & 2) && (b[j].typeBits & 2)) {
                const SliderParts pa = sliderParts(a[i].tail), pb = sliderParts(b[j].tail);
                if (pa.curve != pb.curve) c.fields.push_back({"curve", Token{pa.curve}, Token{pb.curve}});
                if (!pa.length.numEquals(pb.length)) c.fields.push_back({"length", pa.length, pb.length});
                if (!pa.slides.numEquals(pb.slides)) c.fields.push_back({"slides", pa.slides, pb.slides});
                if (pa.edges != pb.edges) c.fields.push_back({"edgeSounds", Token{pa.edges}, Token{pb.edges}});
                if (!sampleListsEqual(pa.sample, pb.sample))
                    c.fields.push_back({"samples", Token{joinTokens(pa.sample, ':')}, Token{joinTokens(pb.sample, ':')}});
            } else if (!sampleListsEqual(a[i].samplesNoEnd(), b[j].samplesNoEnd())) {
                c.fields.push_back({"samples", Token{joinTokens(a[i].samplesNoEnd(), ':')},
                                    Token{joinTokens(b[j].samplesNoEnd(), ':')}});
            }
            // Object position: std keys notes by time only, so a moved circle/
            // slider (its x/y) is a modification, not a move. std compares x+y;
            // catch its x (the lane); taiko has no position and mania encodes the
            // column in x (already the note key), so neither compares it here.
            if (mode == 0) {
                if (!a[i].x.numEquals(b[j].x)) c.fields.push_back({"x", a[i].x, b[j].x});
                if (!a[i].y.numEquals(b[j].y)) c.fields.push_back({"y", a[i].y, b[j].y});
            } else if (mode == 2) {
                if (!a[i].x.numEquals(b[j].x)) c.fields.push_back({"x", a[i].x, b[j].x});
            }
            if (!c.fields.empty()) out.push_back(std::move(c));
            ++i, ++j;
        }
    }
    return out;
}

// Same timestamp, same payload, different column: render as a move. Greedy
// one-to-one pairing in sorted order; merge never sees this decoration.
void pairMoves(std::vector<NoteChange>& notes, int mode)
{
    size_t start = 0;
    while (start < notes.size()) {
        size_t end = start;
        while (end < notes.size() && notes[end].timeMs == notes[start].timeMs) ++end;
        for (size_t r = start; r < end; ++r) {
            if (notes[r].op != ChangeOp::Removed || notes[r].moveSuppressed) continue;
            for (size_t ad = start; ad < end; ++ad) {
                if (notes[ad].op != ChangeOp::Added || notes[ad].movedFromColumn >= 0) continue;
                if (notePayloadEquals(notes[r].before, notes[ad].after, mode)) {
                    notes[ad].movedFromColumn = notes[r].column;
                    notes[r].moveSuppressed = true;
                    break;
                }
            }
        }
        start = end;
    }
}

EventsDiff diffEvents(const CanonicalMap& before, const CanonicalMap& after)
{
    EventsDiff d;
    auto tokenOf = [](const std::optional<Token>& t) { return t.value_or(Token{}); };
    if (!(tokenOf(before.backgroundFile).raw == tokenOf(after.backgroundFile).raw))
        d.background = {"background", tokenOf(before.backgroundFile),
                        tokenOf(after.backgroundFile)};
    if (!(tokenOf(before.videoFile).raw == tokenOf(after.videoFile).raw))
        d.video = {"video", tokenOf(before.videoFile), tokenOf(after.videoFile)};

    size_t i = 0, j = 0;
    const auto& a = before.breaks;
    const auto& b = after.breaks;
    while (i < a.size() || j < b.size()) {
        if (j >= b.size() || (i < a.size() && a[i].startMs < b[j].startMs)) {
            d.breaks.push_back({ChangeOp::Removed, a[i], {}});
            ++i;
        }
        else if (i >= a.size() || b[j].startMs < a[i].startMs) {
            d.breaks.push_back({ChangeOp::Added, {}, b[j]});
            ++j;
        }
        else {
            if (a[i].endMs != b[j].endMs) d.breaks.push_back({ChangeOp::Modified, a[i], b[j]});
            ++i, ++j;
        }
    }

    std::unordered_map<std::string, int> counts;
    for (const std::string& l : before.storyboardLines) ++counts[l];
    for (const std::string& l : after.storyboardLines) --counts[l];
    for (const auto& [line, n] : counts) {
        if (n > 0) d.sbLinesRemoved += n;
        if (n < 0) d.sbLinesAdded += -n;
    }
    d.storyboardChanged = d.sbLinesAdded || d.sbLinesRemoved;
    return d;
}

} // namespace

const char* sectionName(SectionId id)
{
    switch (id) {
    case SectionId::General: return "General";
    case SectionId::Editor: return "Editor";
    case SectionId::Metadata: return "Metadata";
    case SectionId::Difficulty: return "Difficulty";
    case SectionId::Events: return "Events";
    case SectionId::TimingPoints: return "TimingPoints";
    case SectionId::Colours: return "Colours";
    case SectionId::HitObjects: return "HitObjects";
    default: return "Unknown";
    }
}

bool EventsDiff::empty() const
{
    return !background && !video && breaks.empty() && !storyboardChanged;
}

bool BeatmapDiff::empty() const
{
    return !modeChanged && !keyCountChanged && kv.empty() && bookmarks.empty() &&
           tags.empty() && events.empty() && timing.empty() && notes.empty();
}

std::string BeatmapDiff::summary() const
{
    std::vector<std::string> parts;

    int added = 0, removed = 0, modified = 0, moved = 0;
    for (const NoteChange& n : notes) {
        if (n.moveSuppressed) continue;
        if (n.movedFromColumn >= 0) ++moved;
        else if (n.op == ChangeOp::Added) ++added;
        else if (n.op == ChangeOp::Removed) ++removed;
        else ++modified;
    }
    if (added || removed || modified || moved) {
        std::string np;
        if (added) np += "+" + std::to_string(added) + " ";
        if (removed) np += "-" + std::to_string(removed) + " ";
        if (modified) np += "~" + std::to_string(modified) + " ";
        if (moved) np += std::to_string(moved) + " moved ";
        parts.push_back(np + "notes");
    }
    if (keyCountChanged)
        parts.push_back(std::to_string(keyCountBefore) + "K->" + std::to_string(keyCountAfter) +
                        "K");
    if (modeChanged) parts.push_back("mode changed");

    if (!timing.empty()) {
        bool anyRed = false, anyGreen = false;
        for (const TimingChange& t : timing) (t.uninherited ? anyRed : anyGreen) = true;
        const char* label = anyRed && anyGreen ? "timing" : anyRed ? "BPM" : "SV";
        parts.push_back(std::to_string(timing.size()) + " " + label);
    }

    for (const KvDiff& d : kv) {
        if (d.section == SectionId::Difficulty) {
            for (const FieldChange& f : d.changes) {
                std::string name = f.key;
                if (name == "HPDrainRate") name = "HP";
                else if (name == "CircleSize") name = "CS";
                else if (name == "OverallDifficulty") name = "OD";
                else if (name == "ApproachRate") name = "AR";
                else if (name == "SliderMultiplier") name = "SVmult";
                parts.push_back(name + " " + f.before.raw + "->" + f.after.raw);
            }
        }
        else if (!d.changes.empty()) {
            std::string sec = sectionName(d.section);
            for (char& c : sec) c = char(tolower(c));
            parts.push_back(std::to_string(d.changes.size()) + " " + sec);
        }
    }

    if (!events.breaks.empty())
        parts.push_back(std::to_string(events.breaks.size()) + " breaks");
    if (events.background) parts.push_back("background");
    if (events.video) parts.push_back("video");
    if (events.storyboardChanged)
        parts.push_back("SB +" + std::to_string(events.sbLinesAdded) + " -" +
                        std::to_string(events.sbLinesRemoved));
    if (!bookmarks.empty())
        parts.push_back(std::to_string(bookmarks.added.size() + bookmarks.removed.size()) +
                        " bookmarks");
    if (!tags.empty())
        parts.push_back(std::to_string(tags.added.size() + tags.removed.size()) + " tags");

    if (parts.empty()) return "no changes";
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += " · ";
        out += parts[i];
    }
    return out;
}

std::pair<int, int> BeatmapDiff::affectedTimeRange() const
{
    int lo = INT_MAX, hi = INT_MIN;
    auto take = [&](int ms) {
        lo = std::min(lo, ms);
        hi = std::max(hi, ms);
    };
    for (const NoteChange& n : notes) {
        if (n.moveSuppressed) continue;
        take(n.timeMs);
        if (n.op != ChangeOp::Removed && n.after.endTimeMs > n.timeMs) take(n.after.endTimeMs);
        if (n.op == ChangeOp::Removed && n.before.endTimeMs > n.timeMs) take(n.before.endTimeMs);
    }
    for (const TimingChange& t : timing) take(int(t.timeQ / 1000));
    for (const BreakChange& b : events.breaks) {
        if (b.op != ChangeOp::Added) take(int(b.before.startMs));
        if (b.op != ChangeOp::Removed) take(int(b.after.startMs));
    }
    if (lo == INT_MAX) return {-1, -1};
    return {lo, hi};
}

BeatmapDiff diffBeatmaps(const CanonicalMap& before, const CanonicalMap& after)
{
    BeatmapDiff d;
    if (const Token* v = after.kv(SectionId::Metadata, "Version")) d.version = v->raw;
    else if (const Token* v2 = before.kv(SectionId::Metadata, "Version")) d.version = v2->raw;

    d.modeChanged = before.mode != after.mode;
    d.keyCountChanged = !d.modeChanged && after.mode == 3 && before.keyCount != after.keyCount;
    d.keyCountBefore = before.keyCount;
    d.keyCountAfter = after.keyCount;

    d.kv = diffKvSections(before, after);
    d.bookmarks = diffSortedInts(before.bookmarks, after.bookmarks);
    d.tags = diffTagSets(before.tagList, after.tagList);
    d.events = diffEvents(before, after);
    d.timing = diffTiming(before, after);

    // A changed identity space (mode or key count) makes per-note comparison
    // meaningless: every column remaps. The flags carry the story instead.
    if (!d.modeChanged && !d.keyCountChanged) {
        d.notes = diffNotes(before, after);
        pairMoves(d.notes, after.mode);
    }
    return d;
}

} // namespace ovc::core
