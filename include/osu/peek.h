#pragma once
#include <QByteArray>
#include <QString>
#include <optional>

namespace ovc::osu {

// Identity fields pulled from the top of a .osu without a full parse.
struct OsuHeader {
    int formatVersion = -1;
    int mode = 0; // osu! omits Mode: on old std-only files
    QString title;
    QString artist;
    QString creator;
    QString version; // difficulty name
    int beatmapId = -1;
    int beatmapSetId = -1;
};

// `head` = the first few KiB of the file (8 is plenty; Metadata ends early).
// nullopt when it doesn't look like a .osu at all.
std::optional<OsuHeader> peekOsuHeader(const QByteArray& head);

} // namespace ovc::osu
