#pragma once
#include <ovccore/canonical.h>
#include <ovccore/diff.h>
#include <string>

namespace ovc::core {

// The canonical JSON schema shared by the localhost API, the WASM module and
// the web viewer. Numeric .osu values are emitted as their verbatim token
// strings (they are display values; nothing downstream does math on them).

std::string diffToJson(const BeatmapDiff& d);

// Compact map payload for the web timeline: keyCount, notes, timing, breaks,
// bookmarks, kiai ranges, background/audio filenames.
std::string mapToJson(const CanonicalMap& m);

} // namespace ovc::core
