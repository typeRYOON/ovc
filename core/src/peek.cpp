#include <ovccore/peek.h>
#include <ovccore/token.h>

namespace ovc::core {

std::optional<OsuHeader> peekOsuHeader(std::string_view head)
{
    const size_t fmt = head.find("osu file format v");
    if (fmt == std::string_view::npos || fmt > 8) return std::nullopt; // BOM allowance only

    OsuHeader h;
    size_t fmtEnd = head.find('\n', fmt);
    if (fmtEnd == std::string_view::npos) fmtEnd = head.size();
    h.formatVersion = Token{trimCopy(head.substr(fmt + 17, fmtEnd - fmt - 17))}.toInt();

    size_t pos = 0;
    while (pos < head.size()) {
        size_t nl = head.find('\n', pos);
        if (nl == std::string_view::npos) nl = head.size();
        const std::string_view line = trimView(head.substr(pos, nl - pos));
        pos = nl + 1;

        if (!line.empty() && line.front() == '[') {
            // Identity lives in General/Metadata; stop at the first data section.
            if (line == "[Events]" || line == "[TimingPoints]" || line == "[HitObjects]") break;
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == 0 || colon == std::string_view::npos) continue;
        const std::string_view key = trimView(line.substr(0, colon));
        const std::string_view value = trimView(line.substr(colon + 1)); // first colon only

        // Prefer the native (unicode) title/artist — osu writes Title before
        // TitleUnicode, so the *Unicode line overrides the ascii one when present.
        if (key == "Mode") h.mode = Token{std::string(value)}.toInt();
        else if (key == "Title") h.title = value;
        else if (key == "TitleUnicode") { if (!value.empty()) h.title = value; }
        else if (key == "Artist") h.artist = value;
        else if (key == "ArtistUnicode") { if (!value.empty()) h.artist = value; }
        else if (key == "Creator") h.creator = value;
        else if (key == "Version") h.version = value;
        else if (key == "BeatmapID") h.beatmapId = Token{std::string(value)}.toInt();
        else if (key == "BeatmapSetID") h.beatmapSetId = Token{std::string(value)}.toInt();
    }
    return h;
}

} // namespace ovc::core
