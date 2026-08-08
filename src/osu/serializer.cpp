#include <osu/serializer.h>

namespace ovc::osu {

namespace {

void appendLine(QByteArray& out, const RawLine& line)
{
    out += line.raw;
    switch (line.eol) {
    case Eol::Crlf: out += "\r\n"; break;
    case Eol::Lf: out += '\n'; break;
    case Eol::None: break;
    }
}

} // namespace

QByteArray serializeOsu(const OsuDocument& doc)
{
    QByteArray out;
    if (doc.hadBom) out += "\xEF\xBB\xBF";

    for (const RawLine& line : doc.preamble) appendLine(out, line);
    for (const Section& sec : doc.sections) {
        appendLine(out, sec.header);
        for (const RawLine& line : sec.lines) appendLine(out, line);
    }
    return out;
}

} // namespace ovc::osu
