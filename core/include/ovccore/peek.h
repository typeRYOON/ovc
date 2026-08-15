#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace ovc::core {

// Identity fields pulled from the top of a .osu without a full parse.
struct OsuHeader {
    int formatVersion = -1;
    int mode = 0; // osu! omits Mode: on old std-only files
    std::string title;  // native: TitleUnicode when present, else Title
    std::string artist; // native: ArtistUnicode when present, else Artist
    std::string creator;
    std::string version; // difficulty name
    int beatmapId = -1;
    int beatmapSetId = -1;
};

// `head` = the first few KiB of the file (8 is plenty; Metadata ends early).
std::optional<OsuHeader> peekOsuHeader(std::string_view head);

} // namespace ovc::core
