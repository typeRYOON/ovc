#include <ovccore/canonical.h>

namespace ovc::core {

namespace {

std::string joinTokens(const std::vector<Token>& tokens, char sep)
{
    std::string out;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i) out += sep;
        out += tokens[i].raw;
    }
    return out;
}

} // namespace

std::string emitCanonical(const CanonicalMap& m)
{
    std::string out;
    auto line = [&out](std::string_view s) {
        out += s;
        out += "\r\n";
    };

    line("osu file format v" + std::to_string(m.formatVersion > 0 ? m.formatVersion : 14));
    line("");

    // [General] / [Editor] use "Key: value"; [Metadata] / [Difficulty] use
    // "Key:value" — matching stable's per-section house style.
    line("[General]");
    for (const auto& [k, v] : m.general) line(k + ": " + v.raw);
    line("");

    line("[Editor]");
    if (!m.bookmarks.empty()) {
        std::string b;
        for (size_t i = 0; i < m.bookmarks.size(); ++i) {
            if (i) b += ',';
            b += std::to_string(m.bookmarks[i]);
        }
        line("Bookmarks: " + b);
    }
    for (const auto& [k, v] : m.editor) line(k + ": " + v.raw);
    line("");

    line("[Metadata]");
    for (const auto& [k, v] : m.metadata) line(k + ":" + v.raw);
    if (!m.tagList.empty()) {
        std::string t;
        for (size_t i = 0; i < m.tagList.size(); ++i) {
            if (i) t += ' ';
            t += m.tagList[i];
        }
        line("Tags:" + t);
    }
    line("");

    line("[Difficulty]");
    for (const auto& [k, v] : m.difficulty) line(k + ":" + v.raw);
    line("");

    line("[Events]");
    line("//Background and Video events");
    if (m.backgroundFile) line("0,0," + m.backgroundFile->raw + ",0,0");
    if (m.videoFile) line("Video,0," + m.videoFile->raw);
    line("//Break Periods");
    for (const BreakPeriod& b : m.breaks) line("2," + b.start.raw + "," + b.end.raw);
    line("//Storyboard Layer 0 (Background)");
    line("//Storyboard Layer 1 (Fail)");
    line("//Storyboard Layer 2 (Pass)");
    line("//Storyboard Layer 3 (Foreground)");
    line("//Storyboard Layer 4 (Overlay)");
    line("//Storyboard Sound Samples");
    for (const std::string& sb : m.storyboardLines) line(sb);
    line("");

    line("[TimingPoints]");
    for (const TimingPoint& tp : m.timing) line(joinTokens(tp.fields, ','));
    line("");
    line(""); // stable emits a double blank before [HitObjects]

    line("[HitObjects]");
    for (const CanonicalNote& n : m.notes) {
        std::string l = n.x.raw + ',' + n.y.raw + ',' + n.time.raw + ',' + n.type.raw + ',' +
                        n.hitSound.raw;
        if (!n.tail.empty()) l += ',' + n.tail;
        line(l);
    }
    return out;
}

} // namespace ovc::core
