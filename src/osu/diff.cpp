#include <osu/diff.h>
#include <QHash>
#include <QSet>
#include <algorithm>

namespace ovc::osu {

namespace {

// Mania gameplay ignores the new-combo bit (4); stable's editor shuffles it
// freely, so comparing it would produce phantom diffs.
int comparableType(const CanonicalNote& n, int mode)
{
    return mode == 3 ? (n.typeBits & ~4) : n.typeBits;
}

QByteArray joinTokens(const QList<Token>& tokens, char sep)
{
    QByteArray out;
    for (int i = 0; i < tokens.size(); ++i) {
        if (i) out += sep;
        out += tokens[i].raw;
    }
    return out;
}

bool sampleListsEqual(const QList<Token>& a, const QList<Token>& b)
{
    // Index-aligned; absent fields count as "0" (filename as ""). Filename is
    // the last populated slot and compares byte-wise.
    const int n = std::max(a.size(), b.size());
    const Token zero{QByteArrayLiteral("0")};
    for (int i = 0; i < n; ++i) {
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

QList<KvDiff> diffKvSections(const CanonicalMap& before, const CanonicalMap& after)
{
    QList<KvDiff> out;
    const QPair<SectionId, QPair<const QList<QPair<QByteArray, Token>>*,
                                 const QList<QPair<QByteArray, Token>>*>>
        sections[] = {
            {SectionId::General, {&before.general, &after.general}},
            {SectionId::Editor, {&before.editor, &after.editor}},
            {SectionId::Metadata, {&before.metadata, &after.metadata}},
            {SectionId::Difficulty, {&before.difficulty, &after.difficulty}},
        };
    for (const auto& [id, lists] : sections) {
        KvDiff d;
        d.section = id;
        QHash<QByteArray, Token> beforeMap;
        for (const auto& [k, v] : *lists.first) beforeMap.insert(k, v);
        QSet<QByteArray> seen;
        for (const auto& [k, v] : *lists.second) {
            seen.insert(k);
            const auto it = beforeMap.constFind(k);
            if (it == beforeMap.constEnd())
                d.changes.append({k, Token{}, v});
            else if (!it->numEquals(v))
                d.changes.append({k, *it, v});
        }
        for (const auto& [k, v] : *lists.first)
            if (!seen.contains(k)) d.changes.append({k, v, Token{}});
        if (!d.changes.isEmpty()) out.append(d);
    }
    return out;
}

ListDiff diffSortedInts(const QList<qint64>& a, const QList<qint64>& b)
{
    ListDiff d;
    int i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        if (j >= b.size() || (i < a.size() && a[i] < b[j]))
            d.removed.append(QByteArray::number(a[i++]));
        else if (i >= a.size() || b[j] < a[i])
            d.added.append(QByteArray::number(b[j++]));
        else
            ++i, ++j;
    }
    return d;
}

ListDiff diffTagSets(const QList<QByteArray>& a, const QList<QByteArray>& b)
{
    ListDiff d;
    const QSet<QByteArray> sa(a.begin(), a.end());
    const QSet<QByteArray> sb(b.begin(), b.end());
    for (const QByteArray& t : b)
        if (!sa.contains(t)) d.added.append(t);
    for (const QByteArray& t : a)
        if (!sb.contains(t)) d.removed.append(t);
    return d;
}

QList<TimingChange> diffTiming(const CanonicalMap& before, const CanonicalMap& after)
{
    static const char* kNames[] = {"time",        "beatLength", "meter",
                                   "sampleSet",   "sampleIndex", "volume",
                                   "uninherited", "effects"};
    QList<TimingChange> out;
    int i = 0, j = 0;
    const auto& a = before.timing;
    const auto& b = after.timing;
    while (i < a.size() || j < b.size()) {
        const bool takeA = j >= b.size() || (i < a.size() && a[i].key < b[j].key);
        const bool takeB = i >= a.size() || (j < b.size() && b[j].key < a[i].key);
        if (takeA) {
            out.append({ChangeOp::Removed, a[i].key.timeQ, a[i].uninherited, {}, a[i], {}});
            ++i;
        }
        else if (takeB) {
            out.append({ChangeOp::Added, b[j].key.timeQ, b[j].uninherited, {}, {}, b[j]});
            ++j;
        }
        else {
            TimingChange c{ChangeOp::Modified, a[i].key.timeQ, a[i].uninherited, {}, a[i], b[j]};
            const int n = std::max(a[i].fields.size(), b[j].fields.size());
            for (int f = 1; f < n; ++f) { // field 0 is the key itself
                if (!a[i].field(f).numEquals(b[j].field(f)))
                    c.fields.append({QByteArray(f < 8 ? kNames[f] : "extra"), a[i].field(f),
                                     b[j].field(f)});
            }
            if (!c.fields.isEmpty()) out.append(c);
            ++i, ++j;
        }
    }
    return out;
}

QList<NoteChange> diffNotes(const CanonicalMap& before, const CanonicalMap& after)
{
    QList<NoteChange> out;
    const int mode = after.mode;
    int i = 0, j = 0;
    const auto& a = before.notes;
    const auto& b = after.notes;
    while (i < a.size() || j < b.size()) {
        const bool takeA = j >= b.size() || (i < a.size() && a[i].key < b[j].key);
        const bool takeB = i >= a.size() || (j < b.size() && b[j].key < a[i].key);
        if (takeA) {
            out.append({ChangeOp::Removed, a[i].timeMs, a[i].column, {}, a[i], {}});
            ++i;
        }
        else if (takeB) {
            out.append({ChangeOp::Added, b[j].timeMs, b[j].column, {}, {}, b[j]});
            ++j;
        }
        else {
            NoteChange c{ChangeOp::Modified, a[i].timeMs, a[i].column, {}, a[i], b[j]};
            if (comparableType(a[i], mode) != comparableType(b[j], mode))
                c.fields.append({QByteArrayLiteral("type"), a[i].type, b[j].type});
            if (a[i].endTimeMs != b[j].endTimeMs) {
                const Token be = a[i].isHold && !a[i].samples.isEmpty() ? a[i].samples[0] : a[i].time;
                const Token af = b[j].isHold && !b[j].samples.isEmpty() ? b[j].samples[0] : b[j].time;
                c.fields.append({QByteArrayLiteral("endTime"), be, af});
            }
            if (!a[i].hitSound.numEquals(b[j].hitSound))
                c.fields.append({QByteArrayLiteral("hitSound"), a[i].hitSound, b[j].hitSound});
            if (!sampleListsEqual(a[i].samplesNoEnd(), b[j].samplesNoEnd()))
                c.fields.append({QByteArrayLiteral("samples"),
                                 Token{joinTokens(a[i].samplesNoEnd(), ':')},
                                 Token{joinTokens(b[j].samplesNoEnd(), ':')}});
            if (!c.fields.isEmpty()) out.append(c);
            ++i, ++j;
        }
    }
    return out;
}

// Same timestamp, same payload, different column: render as a move. Greedy
// one-to-one pairing in sorted order; merge never sees this decoration.
void pairMoves(QList<NoteChange>& notes, int mode)
{
    int start = 0;
    while (start < notes.size()) {
        int end = start;
        while (end < notes.size() && notes[end].timeMs == notes[start].timeMs) ++end;
        for (int r = start; r < end; ++r) {
            if (notes[r].op != ChangeOp::Removed || notes[r].moveSuppressed) continue;
            for (int ad = start; ad < end; ++ad) {
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
        d.background = {QByteArrayLiteral("background"), tokenOf(before.backgroundFile),
                        tokenOf(after.backgroundFile)};
    if (!(tokenOf(before.videoFile).raw == tokenOf(after.videoFile).raw))
        d.video = {QByteArrayLiteral("video"), tokenOf(before.videoFile),
                   tokenOf(after.videoFile)};

    int i = 0, j = 0;
    const auto& a = before.breaks;
    const auto& b = after.breaks;
    while (i < a.size() || j < b.size()) {
        if (j >= b.size() || (i < a.size() && a[i].startMs < b[j].startMs)) {
            d.breaks.append({ChangeOp::Removed, a[i], {}});
            ++i;
        }
        else if (i >= a.size() || b[j].startMs < a[i].startMs) {
            d.breaks.append({ChangeOp::Added, {}, b[j]});
            ++j;
        }
        else {
            if (a[i].endMs != b[j].endMs) d.breaks.append({ChangeOp::Modified, a[i], b[j]});
            ++i, ++j;
        }
    }

    // Storyboard lines stay opaque in v0.1: multiset compare, counts only.
    QHash<QByteArray, int> counts;
    for (const QByteArray& l : before.storyboardLines) ++counts[l];
    for (const QByteArray& l : after.storyboardLines) --counts[l];
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        if (it.value() > 0) d.sbLinesRemoved += it.value();
        if (it.value() < 0) d.sbLinesAdded += -it.value();
    }
    d.storyboardChanged = d.sbLinesAdded || d.sbLinesRemoved;
    return d;
}

} // namespace

bool EventsDiff::isEmpty() const
{
    return !background && !video && breaks.isEmpty() && !storyboardChanged;
}

bool BeatmapDiff::isEmpty() const
{
    return !modeChanged && !keyCountChanged && kv.isEmpty() && bookmarks.isEmpty() &&
           tags.isEmpty() && events.isEmpty() && timing.isEmpty() && notes.isEmpty();
}

QString BeatmapDiff::summary() const
{
    QStringList parts;

    int added = 0, removed = 0, modified = 0, moved = 0;
    for (const NoteChange& n : notes) {
        if (n.moveSuppressed) continue;
        if (n.movedFromColumn >= 0) ++moved;
        else if (n.op == ChangeOp::Added) ++added;
        else if (n.op == ChangeOp::Removed) ++removed;
        else ++modified;
    }
    if (added || removed || modified || moved) {
        QStringList np;
        if (added) np << QStringLiteral("+%1").arg(added);
        if (removed) np << QStringLiteral("−%1").arg(removed);
        if (modified) np << QStringLiteral("~%1").arg(modified);
        if (moved) np << QStringLiteral("%1 moved").arg(moved);
        parts << np.join(' ') + QStringLiteral(" notes");
    }
    if (keyCountChanged)
        parts << QStringLiteral("%1K→%2K").arg(keyCountBefore).arg(keyCountAfter);
    if (modeChanged) parts << QStringLiteral("mode changed");

    if (!timing.isEmpty()) {
        bool anyRed = false, anyGreen = false;
        for (const TimingChange& t : timing) (t.uninherited ? anyRed : anyGreen) = true;
        const QString label = anyRed && anyGreen ? QStringLiteral("timing")
                              : anyRed          ? QStringLiteral("BPM")
                                                : QStringLiteral("SV");
        parts << QStringLiteral("%1 %2").arg(timing.size()).arg(label);
    }

    for (const KvDiff& d : kv) {
        if (d.section == SectionId::Difficulty) {
            for (const FieldChange& f : d.changes) {
                static const QHash<QByteArray, QByteArray> kShort = {
                    {"HPDrainRate", "HP"},
                    {"CircleSize", "CS"},
                    {"OverallDifficulty", "OD"},
                    {"ApproachRate", "AR"},
                    {"SliderMultiplier", "SVmult"},
                };
                const QByteArray name = kShort.value(f.key, f.key);
                parts << QStringLiteral("%1 %2→%3")
                             .arg(QString::fromUtf8(name), f.before.text(), f.after.text());
            }
        }
        else if (!d.changes.isEmpty()) {
            static const QHash<int, QByteArray> kSecName = {
                {int(SectionId::General), "general"},
                {int(SectionId::Editor), "editor"},
                {int(SectionId::Metadata), "metadata"},
            };
            parts << QStringLiteral("%1 %2")
                         .arg(d.changes.size())
                         .arg(QString::fromUtf8(kSecName.value(int(d.section), "kv")));
        }
    }

    if (!events.breaks.isEmpty())
        parts << QStringLiteral("%1 breaks").arg(events.breaks.size());
    if (events.background) parts << QStringLiteral("background");
    if (events.video) parts << QStringLiteral("video");
    if (events.storyboardChanged)
        parts << QStringLiteral("SB +%1 −%2").arg(events.sbLinesAdded).arg(events.sbLinesRemoved);
    if (!bookmarks.isEmpty())
        parts << QStringLiteral("%1 bookmarks").arg(bookmarks.added.size() + bookmarks.removed.size());
    if (!tags.isEmpty())
        parts << QStringLiteral("%1 tags").arg(tags.added.size() + tags.removed.size());

    return parts.isEmpty() ? QStringLiteral("no changes") : parts.join(QStringLiteral(" · "));
}

QPair<int, int> BeatmapDiff::affectedTimeRange() const
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
    for (const TimingChange& t : timing) take(static_cast<int>(t.timeQ / 1000));
    for (const BreakChange& b : events.breaks) {
        if (b.op != ChangeOp::Added) take(static_cast<int>(b.before.startMs));
        if (b.op != ChangeOp::Removed) take(static_cast<int>(b.after.startMs));
    }
    if (lo == INT_MAX) return {-1, -1};
    return {lo, hi};
}

BeatmapDiff diffBeatmaps(const CanonicalMap& before, const CanonicalMap& after)
{
    BeatmapDiff d;
    if (const Token* v = after.kv(SectionId::Metadata, "Version")) d.version = v->text();
    else if (const Token* v2 = before.kv(SectionId::Metadata, "Version")) d.version = v2->text();

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

} // namespace ovc::osu
