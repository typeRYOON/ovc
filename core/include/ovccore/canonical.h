#pragma once
#include <ovccore/document.h>
#include <ovccore/token.h>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace ovc::core {

// Semantic layer: sorted, keyed views for diff/merge. Built from a parsed
// document, never mutates it; unchanged values keep their verbatim tokens.

struct TimingPoint {
    // time,beatLength,meter,sampleSet,sampleIndex,volume,uninherited,effects —
    // old formats may have fewer fields; field() returns empty then.
    std::vector<Token> fields;
    double timeMs = 0;
    bool uninherited = false;

    struct Key {
        int64_t timeQ = 0; // llround(timeMs*1000): v128 fractional times stay distinct
        int redRank = 0;   // 0 = uninherited (red), 1 = inherited — red applies first
        int occurrence = 0;
        auto operator<=>(const Key&) const = default;
    } key;

    const Token& field(int i) const;
    double beatLength() const { return field(1).toDouble(); }
    double sv() const;  // inherited: 100 / -beatLength; 1.0 otherwise
    double bpm() const; // uninherited: 60000 / beatLength; 0 otherwise
    bool kiai() const { return field(7).toInt() & 1; }
};

struct BreakPeriod {
    Token start, end;
    int64_t startMs = 0, endMs = 0;
};

struct CanonicalNote {
    Token x, y, time, type, hitSound;
    std::string tail; // after the 5th comma, verbatim (empty on old formats)

    int timeMs = 0;
    int typeBits = 0;
    bool isHold = false;
    int endTimeMs = 0;
    int column = 0;
    std::vector<Token> samples;

    struct Key {
        int timeMs = 0;
        int column = 0;
        int occurrence = 0;
        auto operator<=>(const Key&) const = default;
    } key;

    std::vector<Token> samplesNoEnd() const;
};

// Re-derive a note's semantic fields (timeMs/typeBits/isHold/endTimeMs/samples/
// column and key.timeMs/key.column) from its raw tokens (x/type/time/tail).
// Mirrors the derivation in canonicalize()'s [HitObjects] loop; the 3-way merge
// calls it after rebuilding a note from fields taken from different sides.
// key.occurrence is left untouched — the caller owns it.
void deriveNoteFields(CanonicalNote& n, int keyCount);

struct CanonicalMap {
    int formatVersion = -1;
    int mode = 0;
    int keyCount = 0; // mania: round(CircleSize), >=1; 0 for other modes

    std::vector<std::pair<std::string, Token>> general, editor, metadata, difficulty;
    std::vector<int64_t> bookmarks; // sorted
    std::vector<std::string> tagList;

    std::optional<Token> backgroundFile, videoFile;
    std::vector<BreakPeriod> breaks;
    std::vector<std::string> storyboardLines;
    std::vector<TimingPoint> timing;
    std::vector<CanonicalNote> notes;

    const Token* kv(SectionId section, std::string_view key) const;
};

CanonicalMap canonicalize(const OsuDocument& doc, std::vector<ParseWarning>* warnings = nullptr);

// Serialize a canonical map back to valid stable-style .osu text. Data lines
// (timing points, hit objects) reconstruct verbatim from their tokens; the
// output is not byte-identical to any input (osu renormalizes on save anyway),
// but canonicalize(emitCanonical(m)) reproduces m. Used by the 3-way merge.
std::string emitCanonical(const CanonicalMap& m);

} // namespace ovc::core
