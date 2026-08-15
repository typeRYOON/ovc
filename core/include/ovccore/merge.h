#pragma once
#include <ovccore/canonical.h>
#include <map>
#include <string>
#include <vector>

namespace ovc::core {

// Semantic 3-way merge. Two mappers who touch different things (disjoint notes,
// disjoint sections, one hitsounds while the other patterns a different area)
// merge cleanly; overlapping edits to the same thing become conflicts.

enum class MergeDomain : uint8_t {
    Kv,
    Bookmarks,
    Tags,
    Timing,
    Breaks,
    Notes,
    Storyboard,
    WholeFile,
};

struct Conflict {
    MergeDomain domain = MergeDomain::Notes;
    SectionId section = SectionId::Unknown; // Kv only
    // Stable identifier, so a resolver can map a choice back to the conflict:
    //   kv:<Section>:<key> · note:<timeMs>:<col> · timing:<timeMs> ·
    //   break:<startMs> · storyboard
    std::string id;
    std::string key; // human-friendly label: kv key, "m:ss.mmm col N", "m:ss.mmm"
    int timeMs = -1; // for grouping in a resolver UI
    int column = -1;
    // Rendered values (empty = the item was absent on that side).
    std::string base, ours, theirs;
};

// Which side a conflict should resolve to. Absent from the map ⇒ ours (the
// safe default, so an unresolved conflict never loses your work).
enum class ResolveSide : uint8_t { Ours, Theirs };
using ResolutionMap = std::map<std::string, ResolveSide>;

struct MergeResult {
    CanonicalMap merged; // ours wins each conflict, so this is always a usable file
    std::vector<Conflict> conflicts;
    bool wholeFileConflict = false; // identity space differs (mode / key count) — bail
    std::string reason;             // set when wholeFileConflict
    bool clean() const { return conflicts.empty() && !wholeFileConflict; }
};

// `resolutions` maps a Conflict.id to the side to keep; anything not listed
// stays ours. Pass {} for the default ours-wins behavior.
MergeResult merge3(const CanonicalMap& base, const CanonicalMap& ours,
                   const CanonicalMap& theirs, const ResolutionMap& resolutions = {});

} // namespace ovc::core
