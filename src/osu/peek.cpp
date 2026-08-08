#include <osu/peek.h>

namespace ovc::osu {

std::optional<OsuHeader> peekOsuHeader(const QByteArray& head)
{
    const qsizetype fmt = head.indexOf("osu file format v");
    if (fmt < 0 || fmt > 8) return std::nullopt; // allow a BOM before it, nothing else

    OsuHeader h;
    qsizetype fmtEnd = head.indexOf('\n', fmt);
    if (fmtEnd < 0) fmtEnd = head.size();
    h.formatVersion = head.mid(fmt + 17, fmtEnd - fmt - 17).trimmed().toInt();

    qsizetype pos = 0;
    while (pos < head.size()) {
        qsizetype nl = head.indexOf('\n', pos);
        if (nl < 0) nl = head.size();
        QByteArray line = head.mid(pos, nl - pos).trimmed();
        pos = nl + 1;

        if (line.startsWith('[')) {
            // Identity lives in General/Metadata; stop at the first data section.
            if (line == "[Events]" || line == "[TimingPoints]" || line == "[HitObjects]") break;
            continue;
        }
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) continue;
        const QByteArray key = line.left(colon).trimmed();
        const QByteArray value = line.mid(colon + 1).trimmed(); // first colon only: Tags may contain ':'

        if (key == "Mode") h.mode = value.toInt();
        else if (key == "Title") h.title = QString::fromUtf8(value);
        else if (key == "Artist") h.artist = QString::fromUtf8(value);
        else if (key == "Creator") h.creator = QString::fromUtf8(value);
        else if (key == "Version") h.version = QString::fromUtf8(value);
        else if (key == "BeatmapID") h.beatmapId = value.toInt();
        else if (key == "BeatmapSetID") h.beatmapSetId = value.toInt();
    }
    return h;
}

} // namespace ovc::osu
