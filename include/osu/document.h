#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

namespace ovc::osu {

// Physical layer: the file as a classified list of verbatim lines. The parser
// only labels lines, it never rewrites them, which is what makes
// serialize(parse(x)) == x hold for arbitrary input — including corrupt files,
// LF-only hand edits, BOMs and future format versions.

enum class Eol : quint8 { Crlf, Lf, None }; // None = last line without terminator

enum class LineKind : quint8 { Blank, Comment, KeyValue, Data, SectionHeader };

struct RawLine {
    QByteArray raw; // exact bytes, no terminator
    Eol eol = Eol::Crlf;
    LineKind kind = LineKind::Data;
};

enum class SectionId : quint8 {
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
    QByteArray name;                // inner text of the header, e.g. "HitObjects"
    RawLine header;                 // the "[HitObjects]" line verbatim
    QList<RawLine> lines;           // every physical line incl. blanks & comments
};

struct OsuDocument {
    bool hadBom = false;            // stable never writes one; tolerated anyway
    QList<RawLine> preamble;        // "osu file format v14" + anything before the first section
    int formatVersion = -1;         // parsed from the preamble; -1 if absent
    QList<Section> sections;        // file order preserved, unknown sections kept
};

struct ParseWarning {
    int lineNo = 0; // 1-based
    QString message;
};

struct ParseResult {
    OsuDocument doc;
    QList<ParseWarning> warnings;
    bool looksLikeOsu = false; // format line present or a known section seen
};

} // namespace ovc::osu
