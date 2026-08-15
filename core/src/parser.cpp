#include <ovccore/parser.h>
#include <ovccore/token.h>

namespace ovc::core {

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";

SectionId sectionIdFor(std::string_view name)
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

LineKind classify(const std::string& raw, bool inKvSection, std::string* headerName)
{
    const std::string_view t = trimView(raw);
    if (t.empty()) return LineKind::Blank;
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
        if (headerName) *headerName = std::string(t.substr(1, t.size() - 2));
        return LineKind::SectionHeader;
    }
    if (raw.rfind("//", 0) == 0) return LineKind::Comment;
    if (inKvSection && raw.find(':') != std::string::npos) return LineKind::KeyValue;
    return LineKind::Data;
}

} // namespace

ParseResult parseOsu(std::string_view bytes)
{
    ParseResult res;
    OsuDocument& doc = res.doc;

    size_t pos = 0;
    if (bytes.substr(0, 3) == kBom) {
        doc.hadBom = true;
        pos = 3;
    }

    Section* current = nullptr;
    int lineNo = 0;
    bool sawFormatLine = false;

    while (pos <= bytes.size()) {
        // Manual line walk: split at '\n', record \r\n vs \n vs unterminated
        // tail. A lone '\r' without '\n' stays inside raw, preserved as-is.
        const size_t nl = bytes.find('\n', pos);
        RawLine line;
        if (nl == std::string_view::npos) {
            if (pos == bytes.size()) break; // input ended exactly on a terminator
            line.raw = std::string(bytes.substr(pos));
            line.eol = Eol::None;
            pos = bytes.size() + 1;
        }
        else if (nl > pos && bytes[nl - 1] == '\r') {
            line.raw = std::string(bytes.substr(pos, nl - pos - 1));
            line.eol = Eol::Crlf;
            pos = nl + 1;
        }
        else {
            line.raw = std::string(bytes.substr(pos, nl - pos));
            line.eol = Eol::Lf;
            pos = nl + 1;
        }
        ++lineNo;

        std::string headerName;
        line.kind = classify(line.raw, current && isKvSection(current->id), &headerName);

        if (line.kind == LineKind::SectionHeader) {
            Section sec;
            sec.id = sectionIdFor(headerName);
            sec.name = headerName;
            sec.header = line;
            if (sec.id == SectionId::Unknown) {
                res.warnings.push_back({lineNo, "unknown section [" + headerName + "]"});
            }
            else {
                res.looksLikeOsu = true;
                for (const Section& existing : doc.sections) {
                    if (existing.id == sec.id) {
                        res.warnings.push_back(
                            {lineNo, "duplicate section [" + headerName + "]"});
                        break;
                    }
                }
            }
            doc.sections.push_back(std::move(sec));
            current = &doc.sections.back();
            continue;
        }

        if (current) {
            current->lines.push_back(std::move(line));
        }
        else {
            const size_t fmt = line.raw.find("osu file format v");
            if (!sawFormatLine && fmt != std::string::npos) {
                Token version{trimCopy(std::string_view(line.raw).substr(fmt + 17))};
                doc.formatVersion = version.toInt();
                sawFormatLine = true;
                res.looksLikeOsu = true;
            }
            doc.preamble.push_back(std::move(line));
        }
    }

    if (!sawFormatLine) res.warnings.push_back({1, "no 'osu file format v' line"});

    return res;
}

std::string serializeOsu(const OsuDocument& doc)
{
    std::string out;
    if (doc.hadBom) out += kBom;

    auto appendLine = [&out](const RawLine& line) {
        out += line.raw;
        switch (line.eol) {
        case Eol::Crlf: out += "\r\n"; break;
        case Eol::Lf: out += '\n'; break;
        case Eol::None: break;
        }
    };

    for (const RawLine& line : doc.preamble) appendLine(line);
    for (const Section& sec : doc.sections) {
        appendLine(sec.header);
        for (const RawLine& line : sec.lines) appendLine(line);
    }
    return out;
}

} // namespace ovc::core
