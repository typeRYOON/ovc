#pragma once
#include <osu/canonical.h>

namespace ovc::osu {

// Structured semantic diff of two canonical maps. Consumed by the diff UI,
// commit-subject generation, and (v0.3) merge conflict presentation.

enum class ChangeOp : quint8 { Added, Removed, Modified };

struct FieldChange {
    QByteArray key; // "beatLength", "hitSound", "endTime", …
    Token before, after;
};

struct KvDiff {
    SectionId section = SectionId::Unknown;
    QList<FieldChange> changes; // Added: before empty; Removed: after empty
};

struct ListDiff {
    QList<QByteArray> added, removed; // rendered values (bookmark ms / tag text)
    bool isEmpty() const { return added.isEmpty() && removed.isEmpty(); }
};

struct TimingChange {
    ChangeOp op = ChangeOp::Modified;
    qint64 timeQ = 0;
    bool uninherited = false;
    QList<FieldChange> fields; // Modified only
    TimingPoint before, after; // whole rows for display
};

struct NoteChange {
    ChangeOp op = ChangeOp::Modified;
    int timeMs = 0;
    int column = 0;
    QList<FieldChange> fields; // Modified only
    CanonicalNote before, after;
    int movedFromColumn = -1;   // set on the Added half of a paired column move
    bool moveSuppressed = false; // set on the Removed half — hide in UI, merge ignores
};

struct BreakChange {
    ChangeOp op = ChangeOp::Modified;
    BreakPeriod before, after;
};

struct EventsDiff {
    std::optional<FieldChange> background, video;
    QList<BreakChange> breaks;
    bool storyboardChanged = false;
    int sbLinesAdded = 0, sbLinesRemoved = 0; // opaque line counts in v0.1
    bool isEmpty() const;
};

struct BeatmapDiff {
    QString version; // difficulty name (after side)
    // When the identity space itself changed, notes[] is intentionally empty.
    bool modeChanged = false;
    bool keyCountChanged = false;
    int keyCountBefore = 0, keyCountAfter = 0;

    QList<KvDiff> kv; // General/Editor/Metadata/Difficulty (minus Bookmarks/Tags)
    ListDiff bookmarks, tags;
    EventsDiff events;
    QList<TimingChange> timing;
    QList<NoteChange> notes;

    bool isEmpty() const;
    QString summary() const;                 // "+12 −3 ~1 notes · 2 SV · OD 8→8.3"
    QPair<int, int> affectedTimeRange() const; // ms; {-1,-1} when nothing timed changed
};

BeatmapDiff diffBeatmaps(const CanonicalMap& before, const CanonicalMap& after);

} // namespace ovc::osu
