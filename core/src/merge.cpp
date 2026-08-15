#include <ovccore/diff.h> // sectionName
#include <ovccore/merge.h>
#include <ovccore/timefmt.h>
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace ovc::core {

namespace {

// The heart of the merge. For one item, given its base/ours/theirs states
// (nullopt = absent), pick the merged state. Uniform across add / remove /
// modify because "absent" is just another state. Conflict when both sides
// diverged from base differently; ours wins so the output stays usable.
template <class T, class Eq>
std::optional<T> resolve3(const std::optional<T>& base, const std::optional<T>& ours,
                          const std::optional<T>& theirs, Eq eq, bool& conflict,
                          bool preferTheirs = false)
{
    conflict = false;
    auto same = [&](const std::optional<T>& a, const std::optional<T>& b) {
        if (a.has_value() != b.has_value()) return false;
        return !a.has_value() || eq(*a, *b);
    };
    if (same(ours, base)) return theirs; // ours didn't touch it → take theirs
    if (same(theirs, base)) return ours; // theirs didn't touch it → take ours
    if (same(ours, theirs)) return ours; // both made the same change
    conflict = true;
    return preferTheirs ? theirs : ours; // resolver's choice, else ours
}

bool wantsTheirs(const ResolutionMap& res, const std::string& id)
{
    const auto it = res.find(id);
    return it != res.end() && it->second == ResolveSide::Theirs;
}

// ---- per-domain equality ----

bool kvEq(const Token& a, const Token& b) { return a.numEquals(b); }

int comparableType(const CanonicalNote& n, int mode) { return mode == 3 ? (n.typeBits & ~4) : n.typeBits; }

// A note splits into three independently-mergeable fields, so two mappers who
// touch different aspects of the SAME note (one re-hitsounds it, the other
// lengthens the LN) merge cleanly instead of colliding. Type and end time are
// one coupled "shape" field: a note-vs-hold flip and an end-time edit can't be
// recombined into a coherent object, so they must conflict together.
bool shapeEq(const CanonicalNote& a, const CanonicalNote& b, int mode)
{
    return comparableType(a, mode) == comparableType(b, mode) && a.isHold == b.isHold &&
           a.endTimeMs == b.endTimeMs;
}

bool hitSoundEq(const CanonicalNote& a, const CanonicalNote& b) { return a.hitSound.numEquals(b.hitSound); }

bool samplesEq(const CanonicalNote& a, const CanonicalNote& b)
{
    const auto sa = a.samplesNoEnd(), sb = b.samplesNoEnd();
    const size_t n = std::max(sa.size(), sb.size());
    for (size_t i = 0; i < n; ++i) {
        const bool file = i == 4;
        const Token ta = i < sa.size() ? sa[i] : Token{file ? "" : "0"};
        const Token tb = i < sb.size() ? sb[i] : Token{file ? "" : "0"};
        if (file ? ta.raw != tb.raw : !ta.numEquals(tb)) return false;
    }
    return true;
}

// Object position — std merges x+y, catch its x (the lane); taiko has no position
// and mania encodes the column in x (already the note key), so both ignore it.
bool posEq(const CanonicalNote& a, const CanonicalNote& b, int mode)
{
    if (mode == 0) return a.x.numEquals(b.x) && a.y.numEquals(b.y);
    if (mode == 2) return a.x.numEquals(b.x);
    return true;
}

bool noteEq(const CanonicalNote& a, const CanonicalNote& b, int mode)
{
    return shapeEq(a, b, mode) && hitSoundEq(a, b) && samplesEq(a, b) && posEq(a, b, mode);
}

bool timingEq(const TimingPoint& a, const TimingPoint& b)
{
    const size_t n = std::max(a.fields.size(), b.fields.size());
    for (size_t i = 0; i < n; ++i)
        if (!a.field(int(i)).numEquals(b.field(int(i)))) return false;
    return true;
}

// ---- renderers for conflict reporting ----

std::string renderNote(const std::optional<CanonicalNote>& n)
{
    if (!n) return {};
    std::string s = std::string(n->isHold ? "hold" : "note");
    s += " hs=" + n->hitSound.raw;
    if (n->isHold) s += " end=" + msToClock(n->endTimeMs);
    return s;
}

std::string renderShape(const std::optional<CanonicalNote>& n)
{
    if (!n) return {};
    return n->isHold ? "hold end=" + msToClock(n->endTimeMs) : std::string("note");
}

std::string renderHitSound(const std::optional<CanonicalNote>& n)
{
    return n ? "hs=" + n->hitSound.raw : std::string();
}

std::string renderSamples(const std::optional<CanonicalNote>& n)
{
    if (!n) return {};
    std::string s;
    for (const Token& t : n->samplesNoEnd()) {
        if (!s.empty()) s += ':';
        s += t.raw;
    }
    return s.empty() ? std::string("(none)") : s;
}

std::string renderPosition(const std::optional<CanonicalNote>& n)
{
    return n ? "x=" + n->x.raw + " y=" + n->y.raw : std::string();
}

std::string renderTiming(const std::optional<TimingPoint>& t)
{
    if (!t) return {};
    std::string s;
    for (size_t i = 0; i < t->fields.size(); ++i) {
        if (i) s += ',';
        s += t->fields[i].raw;
    }
    return s;
}

// ---- keyed 3-way over a domain, appending to `out` and collecting conflicts ----

template <class T, class KeyOf, class Eq, class Render, class IdOf>
void mergeKeyed(const std::vector<T>& base, const std::vector<T>& ours,
                const std::vector<T>& theirs, KeyOf keyOf, Eq eq, MergeDomain domain,
                Render render, IdOf idOf, const ResolutionMap& res, std::vector<T>& out,
                std::vector<Conflict>& conflicts)
{
    using Key = decltype(keyOf(base.front()));
    std::map<Key, T> b, o, t;
    for (const T& x : base) b.emplace(keyOf(x), x);
    for (const T& x : ours) o.emplace(keyOf(x), x);
    for (const T& x : theirs) t.emplace(keyOf(x), x);

    std::set<Key> keys;
    for (const auto& [k, _] : b) keys.insert(k);
    for (const auto& [k, _] : o) keys.insert(k);
    for (const auto& [k, _] : t) keys.insert(k);

    for (const Key& k : keys) {
        auto pick = [&](const std::map<Key, T>& m) -> std::optional<T> {
            const auto it = m.find(k);
            return it == m.end() ? std::nullopt : std::optional<T>(it->second);
        };
        const std::string id = idOf(k);
        bool conflict = false;
        const auto merged =
            resolve3(pick(b), pick(o), pick(t), eq, conflict, wantsTheirs(res, id));
        if (merged) out.push_back(*merged);
        if (conflict) {
            Conflict c;
            c.domain = domain;
            c.id = id;
            render(k, pick(b), pick(o), pick(t), c);
            conflicts.push_back(std::move(c));
        }
    }
}

// ---- set merge (bookmarks, tags): presence-only, never conflicts ----

template <class T>
std::vector<T> mergeSet(const std::vector<T>& base, const std::vector<T>& ours,
                        const std::vector<T>& theirs)
{
    std::set<T> b(base.begin(), base.end());
    std::set<T> o(ours.begin(), ours.end());
    std::set<T> t(theirs.begin(), theirs.end());
    std::set<T> all;
    all.insert(b.begin(), b.end());
    all.insert(o.begin(), o.end());
    all.insert(t.begin(), t.end());

    std::vector<T> out;
    for (const T& e : all) {
        const std::optional<T> be = b.count(e) ? std::optional<T>(e) : std::nullopt;
        const std::optional<T> oe = o.count(e) ? std::optional<T>(e) : std::nullopt;
        const std::optional<T> te = t.count(e) ? std::optional<T>(e) : std::nullopt;
        bool conflict = false;
        if (resolve3(be, oe, te, [](const T&, const T&) { return true; }, conflict))
            out.push_back(e);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void mergeKvSection(SectionId section,
                    const std::vector<std::pair<std::string, Token>>& base,
                    const std::vector<std::pair<std::string, Token>>& ours,
                    const std::vector<std::pair<std::string, Token>>& theirs,
                    const ResolutionMap& res,
                    std::vector<std::pair<std::string, Token>>& out,
                    std::vector<Conflict>& conflicts)
{
    auto asMap = [](const std::vector<std::pair<std::string, Token>>& v) {
        std::unordered_map<std::string, Token> m;
        for (const auto& [k, t] : v) m.emplace(k, t);
        return m;
    };
    const auto b = asMap(base), o = asMap(ours), t = asMap(theirs);
    // Preserve ours' key order, then append keys theirs added.
    std::vector<std::string> order;
    std::set<std::string> seen;
    auto addKeys = [&](const std::vector<std::pair<std::string, Token>>& v) {
        for (const auto& [k, _] : v)
            if (seen.insert(k).second) order.push_back(k);
    };
    addKeys(ours);
    addKeys(theirs);
    addKeys(base);

    for (const std::string& k : order) {
        auto pick = [&](const std::unordered_map<std::string, Token>& m) -> std::optional<Token> {
            const auto it = m.find(k);
            return it == m.end() ? std::nullopt : std::optional<Token>(it->second);
        };
        const std::string id = std::string("kv:") + sectionName(section) + ":" + k;
        bool conflict = false;
        const auto merged =
            resolve3(pick(b), pick(o), pick(t), kvEq, conflict, wantsTheirs(res, id));
        if (merged) out.emplace_back(k, *merged);
        if (conflict) {
            Conflict c;
            c.domain = MergeDomain::Kv;
            c.section = section;
            c.id = id;
            c.key = k;
            c.base = pick(b) ? pick(b)->raw : std::string();
            c.ours = pick(o) ? pick(o)->raw : std::string();
            c.theirs = pick(t) ? pick(t)->raw : std::string();
            conflicts.push_back(std::move(c));
        }
    }
}

// Rebuild a hit-object tail from a resolved shape (hold-ness + end time) and the
// resolved sample fields — which may come from different sides. Mirrors the .osu
// tail grammar: [endTime:]normalSet:additionSet:index:volume:filename.
std::string buildTail(const CanonicalNote& shape, const std::vector<Token>& samplesNoEnd)
{
    std::vector<std::string> parts;
    if (shape.isHold)
        parts.push_back(shape.samples.empty() ? std::to_string(shape.endTimeMs)
                                              : shape.samples[0].raw);
    for (const Token& t : samplesNoEnd) parts.push_back(t.raw);
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += ':';
        out += parts[i];
    }
    return out;
}

// Notes merge per-field (shape / hitsound / samples). Add or delete of a whole
// note stays note-granular — a per-field merge only means something when the note
// exists on both sides.
void mergeNotes(const std::vector<CanonicalNote>& base, const std::vector<CanonicalNote>& ours,
                const std::vector<CanonicalNote>& theirs, int mode, int keyCount,
                const ResolutionMap& res, std::vector<CanonicalNote>& out,
                std::vector<Conflict>& conflicts)
{
    using Key = CanonicalNote::Key;
    std::map<Key, CanonicalNote> b, o, t;
    for (const CanonicalNote& x : base) b.emplace(x.key, x);
    for (const CanonicalNote& x : ours) o.emplace(x.key, x);
    for (const CanonicalNote& x : theirs) t.emplace(x.key, x);

    std::set<Key> keys;
    for (const auto& [k, _] : b) keys.insert(k);
    for (const auto& [k, _] : o) keys.insert(k);
    for (const auto& [k, _] : t) keys.insert(k);

    for (const Key& k : keys) {
        auto pick = [&](const std::map<Key, CanonicalNote>& m) -> std::optional<CanonicalNote> {
            const auto it = m.find(k);
            return it == m.end() ? std::nullopt : std::optional<CanonicalNote>(it->second);
        };
        const std::optional<CanonicalNote> bn = pick(b), on = pick(o), tn = pick(t);
        if (!on && !tn) continue; // removed on both sides

        const std::string id = "note:" + std::to_string(k.timeMs) + ":" +
                               std::to_string(k.column) + ":" + std::to_string(k.occurrence);
        const std::string label = msToClock(k.timeMs) + " col " + std::to_string(k.column);

        // Add, or delete-vs-modify: resolved at whole-note granularity (a per-field
        // merge needs the note on both sides).
        if (!on || !tn) {
            bool conflict = false;
            const auto merged = resolve3(
                bn, on, tn,
                [mode](const CanonicalNote& a, const CanonicalNote& c) { return noteEq(a, c, mode); },
                conflict, wantsTheirs(res, id));
            if (merged) out.push_back(*merged);
            if (conflict) {
                Conflict c;
                c.domain = MergeDomain::Notes;
                c.id = id;
                c.timeMs = k.timeMs;
                c.column = k.column;
                c.key = label;
                c.base = renderNote(bn);
                c.ours = renderNote(on);
                c.theirs = renderNote(tn);
                conflicts.push_back(std::move(c));
            }
            continue;
        }

        // Present on both sides. Fast paths keep the winning side's tokens verbatim
        // and skip the rebuild entirely for the common cases.
        if (bn && noteEq(*on, *bn, mode)) { out.push_back(*tn); continue; } // ours untouched → theirs
        if (bn && noteEq(*tn, *bn, mode)) { out.push_back(*on); continue; } // theirs untouched → ours
        if (noteEq(*on, *tn, mode)) { out.push_back(*on); continue; }       // same edit both sides

        // Genuine divergence: resolve each field on its own, then reassemble.
        bool cShape = false, cHs = false, cSm = false, cPos = false;
        const auto shape = resolve3(
            bn, on, tn,
            [mode](const CanonicalNote& a, const CanonicalNote& c) { return shapeEq(a, c, mode); }, cShape,
            wantsTheirs(res, id + ":shape"));
        const auto hs = resolve3(bn, on, tn, hitSoundEq, cHs, wantsTheirs(res, id + ":hitsound"));
        const auto sm = resolve3(bn, on, tn, samplesEq, cSm, wantsTheirs(res, id + ":samples"));
        // Position is a field too (std x+y / catch x); for taiko/mania it never
        // differs (posEq is always true), so it resolves to ours and never conflicts.
        const bool hasPos = mode == 0 || mode == 2;
        const auto pos = hasPos
            ? resolve3(bn, on, tn,
                       [mode](const CanonicalNote& a, const CanonicalNote& c) { return posEq(a, c, mode); },
                       cPos, wantsTheirs(res, id + ":position"))
            : on;
        // on && tn both present ⇒ each resolve3 returns one of them, never absent.

        CanonicalNote merged = *on; // x / y / time / key template (identical key on both sides)
        if (hasPos && pos) { merged.x = pos->x; merged.y = pos->y; }
        merged.type = shape->type;
        merged.hitSound = hs->hitSound;
        merged.tail = buildTail(*shape, sm->samplesNoEnd());
        deriveNoteFields(merged, keyCount); // typeBits/isHold/endTimeMs/samples/column from tokens
        merged.key.occurrence = k.occurrence;
        out.push_back(std::move(merged));

        auto report = [&](bool conflict, const char* field, const char* fieldLabel,
                          std::string (*render)(const std::optional<CanonicalNote>&)) {
            if (!conflict) return;
            Conflict c;
            c.domain = MergeDomain::Notes;
            c.id = id + ":" + field;
            c.timeMs = k.timeMs;
            c.column = k.column;
            c.key = label + " \xc2\xb7 " + fieldLabel; // "· <field>"
            c.base = render(bn);
            c.ours = render(on);
            c.theirs = render(tn);
            conflicts.push_back(std::move(c));
        };
        report(cShape, "shape", "shape", renderShape);
        report(cHs, "hitsound", "hitsound", renderHitSound);
        report(cSm, "samples", "samples", renderSamples);
        report(cPos, "position", "position", renderPosition);
    }
}

} // namespace

MergeResult merge3(const CanonicalMap& base, const CanonicalMap& ours, const CanonicalMap& theirs,
                   const ResolutionMap& res)
{
    MergeResult r;

    // Identity space must agree, or column/timeline keys mean different things
    // on each side and a keyed merge produces garbage.
    if (ours.mode != theirs.mode) {
        r.wholeFileConflict = true;
        r.reason = "game mode differs between the two versions";
        return r;
    }
    if (ours.mode == 3 && ours.keyCount != theirs.keyCount) {
        r.wholeFileConflict = true;
        r.reason = "key count (CircleSize) differs between the two versions";
        return r;
    }

    r.merged.formatVersion = ours.formatVersion;
    r.merged.mode = ours.mode;
    r.merged.keyCount = ours.keyCount;

    mergeKvSection(SectionId::General, base.general, ours.general, theirs.general, res,
                   r.merged.general, r.conflicts);
    mergeKvSection(SectionId::Editor, base.editor, ours.editor, theirs.editor, res,
                   r.merged.editor, r.conflicts);
    mergeKvSection(SectionId::Metadata, base.metadata, ours.metadata, theirs.metadata, res,
                   r.merged.metadata, r.conflicts);
    mergeKvSection(SectionId::Difficulty, base.difficulty, ours.difficulty, theirs.difficulty,
                   res, r.merged.difficulty, r.conflicts);

    r.merged.bookmarks = mergeSet(base.bookmarks, ours.bookmarks, theirs.bookmarks);
    r.merged.tagList = mergeSet(base.tagList, ours.tagList, theirs.tagList);

    // Background / video: single optional values.
    {
        auto opt = [](const std::optional<Token>& t) { return t; };
        const std::string bgId = "kv:Events:background";
        bool conflict = false;
        r.merged.backgroundFile =
            resolve3(opt(base.backgroundFile), opt(ours.backgroundFile),
                     opt(theirs.backgroundFile), kvEq, conflict, wantsTheirs(res, bgId));
        if (conflict) {
            Conflict c;
            c.domain = MergeDomain::Kv;
            c.section = SectionId::Events;
            c.id = bgId;
            c.key = "background";
            c.base = base.backgroundFile ? base.backgroundFile->raw : std::string();
            c.ours = ours.backgroundFile ? ours.backgroundFile->raw : std::string();
            c.theirs = theirs.backgroundFile ? theirs.backgroundFile->raw : std::string();
            r.conflicts.push_back(std::move(c));
        }
        conflict = false;
        r.merged.videoFile =
            resolve3(opt(base.videoFile), opt(ours.videoFile), opt(theirs.videoFile), kvEq,
                     conflict, wantsTheirs(res, "kv:Events:video"));
    }

    mergeKeyed(
        base.breaks, ours.breaks, theirs.breaks, [](const BreakPeriod& b) { return b.startMs; },
        [](const BreakPeriod& a, const BreakPeriod& b) { return a.endMs == b.endMs; },
        MergeDomain::Breaks,
        [](int64_t k, const std::optional<BreakPeriod>& b, const std::optional<BreakPeriod>& o,
           const std::optional<BreakPeriod>& t, Conflict& c) {
            c.timeMs = int(k);
            c.key = msToClock(k);
            c.base = b ? msToClock(b->endMs) : std::string();
            c.ours = o ? msToClock(o->endMs) : std::string();
            c.theirs = t ? msToClock(t->endMs) : std::string();
        },
        [](int64_t k) { return "break:" + std::to_string(k); }, res, r.merged.breaks,
        r.conflicts);
    std::stable_sort(r.merged.breaks.begin(), r.merged.breaks.end(),
                     [](const BreakPeriod& a, const BreakPeriod& b) { return a.startMs < b.startMs; });

    mergeKeyed(
        base.timing, ours.timing, theirs.timing, [](const TimingPoint& t) { return t.key; },
        timingEq, MergeDomain::Timing,
        [](const TimingPoint::Key& k, const std::optional<TimingPoint>& b,
           const std::optional<TimingPoint>& o, const std::optional<TimingPoint>& t, Conflict& c) {
            c.timeMs = int(k.timeQ / 1000);
            c.key = msToClock(k.timeQ / 1000);
            c.base = renderTiming(b);
            c.ours = renderTiming(o);
            c.theirs = renderTiming(t);
        },
        [](const TimingPoint::Key& k) {
            return "timing:" + std::to_string(k.timeQ) + ":" + std::to_string(k.redRank);
        },
        res, r.merged.timing, r.conflicts);
    std::stable_sort(r.merged.timing.begin(), r.merged.timing.end(),
                     [](const TimingPoint& a, const TimingPoint& b) { return a.key < b.key; });

    mergeNotes(base.notes, ours.notes, theirs.notes, ours.mode, ours.keyCount, res, r.merged.notes,
               r.conflicts);
    std::stable_sort(r.merged.notes.begin(), r.merged.notes.end(),
                     [](const CanonicalNote& a, const CanonicalNote& b) { return a.key < b.key; });

    // Storyboard block: opaque line list, merged as a set of lines (order is
    // rebuilt on emit). A both-sides-different case is rare; treat the whole
    // block as ours-wins with a single conflict marker.
    if (ours.storyboardLines != theirs.storyboardLines &&
        ours.storyboardLines != base.storyboardLines &&
        theirs.storyboardLines != base.storyboardLines) {
        r.merged.storyboardLines =
            wantsTheirs(res, "storyboard") ? theirs.storyboardLines : ours.storyboardLines;
        Conflict c;
        c.domain = MergeDomain::Storyboard;
        c.id = "storyboard";
        c.key = "storyboard";
        c.base = std::to_string(base.storyboardLines.size()) + " lines";
        c.ours = std::to_string(ours.storyboardLines.size()) + " lines";
        c.theirs = std::to_string(theirs.storyboardLines.size()) + " lines";
        r.conflicts.push_back(std::move(c));
    }
    else {
        r.merged.storyboardLines =
            ours.storyboardLines == base.storyboardLines ? theirs.storyboardLines
                                                         : ours.storyboardLines;
    }

    return r;
}

} // namespace ovc::core
