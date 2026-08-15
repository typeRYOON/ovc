#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ovc::core {

// Physical layer: the file as a classified list of verbatim lines. The parser
// only labels lines, it never rewrites them, which is what makes
// serialize(parse(x)) == x hold for arbitrary input — including corrupt files,
// LF-only hand edits, BOMs and future format versions.

enum class Eol : uint8_t { Crlf, Lf, None }; // None = last line without terminator

enum class LineKind : uint8_t { Blank, Comment, KeyValue, Data, SectionHeader };

struct RawLine {
    std::string raw; // exact bytes, no terminator
    Eol eol = Eol::Crlf;
    LineKind kind = LineKind::Data;
};

enum class SectionId : uint8_t {
    General,
    Editor,
    Metadata,
    Difficulty,
    Events,
    TimingPoints,
    Colours,
    HitObjects,
    Unknown,
};

struct Section {
    SectionId id = SectionId::Unknown;
    std::string name;             // inner text of the header, e.g. "HitObjects"
    RawLine header;               // the "[HitObjects]" line verbatim
    std::vector<RawLine> lines;   // every physical line incl. blanks & comments
};

struct OsuDocument {
    bool hadBom = false;          // stable never writes one; tolerated anyway
    std::vector<RawLine> preamble;
    int formatVersion = -1;       // parsed from the preamble; -1 if absent
    std::vector<Section> sections;
};

struct ParseWarning {
    int lineNo = 0; // 1-based
    std::string message;
};

struct ParseResult {
    OsuDocument doc;
    std::vector<ParseWarning> warnings;
    bool looksLikeOsu = false;
};

} // namespace ovc::core
