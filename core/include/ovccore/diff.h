#pragma once
#include <ovccore/canonical.h>

namespace ovc::core {

// Structured semantic diff of two canonical maps. One schema serves the CLI,
// the localhost API and the web viewer (see json.h).

enum class ChangeOp : uint8_t { Added, Removed, Modified };

struct FieldChange {
    std::string key; // "beatLength", "hitSound", "endTime", …
    Token before, after;
};

struct KvDiff {
    SectionId section = SectionId::Unknown;
    std::vector<FieldChange> changes;
};

struct ListDiff {
    std::vector<std::string> added, removed;
    bool empty() const { return added.empty() && removed.empty(); }
};

struct TimingChange {
    ChangeOp op = ChangeOp::Modified;
    int64_t timeQ = 0;
    bool uninherited = false;
    std::vector<FieldChange> fields; // Modified only
    TimingPoint before, after;
};

struct NoteChange {
    ChangeOp op = ChangeOp::Modified;
    int timeMs = 0;
    int column = 0;
    std::vector<FieldChange> fields;
    CanonicalNote before, after;
    int movedFromColumn = -1;    // set on the Added half of a paired column move
    bool moveSuppressed = false; // set on the Removed half — hide in UI, merge ignores
};

struct BreakChange {
    ChangeOp op = ChangeOp::Modified;
    BreakPeriod before, after;
};

struct EventsDiff {
    std::optional<FieldChange> background, video;
    std::vector<BreakChange> breaks;
    bool storyboardChanged = false;
    int sbLinesAdded = 0, sbLinesRemoved = 0;
    bool empty() const;
};

struct BeatmapDiff {
    std::string version; // difficulty name (after side)
    bool modeChanged = false;
    bool keyCountChanged = false;
    int keyCountBefore = 0, keyCountAfter = 0;

    std::vector<KvDiff> kv;
    ListDiff bookmarks, tags;
    EventsDiff events;
    std::vector<TimingChange> timing;
    std::vector<NoteChange> notes;

    bool empty() const;
    std::string summary() const;                  // "+12 -3 ~1 notes · 2 SV · OD 8→8.3"
    std::pair<int, int> affectedTimeRange() const; // {-1,-1} when nothing timed changed
};

BeatmapDiff diffBeatmaps(const CanonicalMap& before, const CanonicalMap& after);

const char* sectionName(SectionId id); // "General", …, "Unknown"

} // namespace ovc::core
