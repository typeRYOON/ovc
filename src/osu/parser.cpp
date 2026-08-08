#include <osu/parser.h>

namespace ovc::osu {

namespace {

const char kBom[] = "\xEF\xBB\xBF";

SectionId sectionIdFor(const QByteArray& name)
{
    if (name == "General") return SectionId::General;
    if (name == "Editor") return SectionId::Editor;
    if (name == "Metadata") return SectionId::Metadata;
    if (name == "Difficulty") return SectionId::Difficulty;
    if (name == "Events") return SectionId::Events;
    if (name == "TimingPoints") return SectionId::TimingPoints;
    if (name == "Colours") return SectionId::Colours;
    if (name == "HitObjects") return SectionId::HitObjects;
    return SectionId::Unknown;
}

// KV shape only exists in these; TimingPoints/HitObjects lines contain ':'
// inside data (hitsound extras), so the section decides, not the line.
bool isKvSection(SectionId id)
{
    switch (id) {
    case SectionId::General:
    case SectionId::Editor:
    case SectionId::Metadata:
    case SectionId::Difficulty:
    case SectionId::Colours: return true;
    default: return false;
    }
}

LineKind classify(const QByteArray& raw, bool inKvSection, QByteArray* headerName)
{
    const QByteArray t = raw.trimmed();
    if (t.isEmpty()) return LineKind::Blank;
    if (t.startsWith('[') && t.endsWith(']')) {
        if (headerName) *headerName = t.mid(1, t.size() - 2);
        return LineKind::SectionHeader;
    }
    if (raw.startsWith("//")) return LineKind::Comment;
    if (inKvSection && raw.contains(':')) return LineKind::KeyValue;
    return LineKind::Data;
}

} // namespace

ParseResult parseOsu(const QByteArray& bytes)
{
    ParseResult res;
    OsuDocument& doc = res.doc;

    qsizetype pos = 0;
    if (bytes.startsWith(kBom)) {
        doc.hadBom = true;
        pos = 3;
    }

    Section* current = nullptr;
    int lineNo = 0;
    bool sawFormatLine = false;

    while (pos <= bytes.size()) {
        // Manual line walk: split at '\n', record \r\n vs \n vs unterminated
        // tail. A lone '\r' without '\n' stays inside raw, preserved as-is.
        const qsizetype nl = bytes.indexOf('\n', pos);
        RawLine line;
        if (nl < 0) {
            if (pos == bytes.size()) break; // input ended exactly on a terminator
            line.raw = bytes.mid(pos);
            line.eol = Eol::None;
            pos = bytes.size() + 1;
        }
        else if (nl > pos && bytes.at(nl - 1) == '\r') {
            line.raw = bytes.mid(pos, nl - pos - 1);
            line.eol = Eol::Crlf;
            pos = nl + 1;
        }
        else {
            line.raw = bytes.mid(pos, nl - pos);
            line.eol = Eol::Lf;
            pos = nl + 1;
        }
        ++lineNo;

        QByteArray headerName;
        line.kind = classify(line.raw, current && isKvSection(current->id), &headerName);

        if (line.kind == LineKind::SectionHeader) {
            Section sec;
            sec.id = sectionIdFor(headerName);
            sec.name = headerName;
            sec.header = line;
            if (sec.id == SectionId::Unknown) {
                res.warnings.append({lineNo, QStringLiteral("unknown section [%1]")
                                                 .arg(QString::fromUtf8(headerName))});
            }
            else {
                res.looksLikeOsu = true;
                for (const Section& existing : doc.sections) {
                    if (existing.id == sec.id) {
                        res.warnings.append(
                            {lineNo, QStringLiteral("duplicate section [%1]")
                                         .arg(QString::fromUtf8(headerName))});
                        break;
                    }
                }
            }
            doc.sections.append(sec);
            current = &doc.sections.last();
            continue;
        }

        if (current) {
            current->lines.append(line);
        }
        else {
            if (!sawFormatLine && line.raw.contains("osu file format v")) {
                const qsizetype v = line.raw.indexOf("osu file format v") + 17;
                doc.formatVersion = line.raw.mid(v).trimmed().toInt();
                sawFormatLine = true;
                res.looksLikeOsu = true;
            }
            doc.preamble.append(line);
        }
    }

    if (!sawFormatLine)
        res.warnings.append({1, QStringLiteral("no 'osu file format v' line")});

    return res;
}

} // namespace ovc::osu
