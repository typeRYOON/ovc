#pragma once
#include <osu/document.h>
#include <osu/token.h>
#include <QList>
#include <QPair>
#include <optional>

namespace ovc::osu {

// Semantic layer: sorted, keyed views for diff/merge. Built from a parsed
// document, never mutates it; unchanged values keep their verbatim tokens.

struct TimingPoint {
    // time,beatLength,meter,sampleSet,sampleIndex,volume,uninherited,effects —
    // old formats may have fewer fields; field() returns empty then.
    QList<Token> fields;
    double timeMs = 0;
    bool uninherited = false;

    struct Key {
        qint64 timeQ = 0;  // llround(timeMs*1000): v128 fractional times stay distinct
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
    qint64 startMs = 0, endMs = 0;
};

struct CanonicalNote {
    Token x, y, time, type, hitSound;
    QByteArray tail; // everything after the 5th comma, verbatim (empty on old formats)

    int timeMs = 0;
    int typeBits = 0;
    bool isHold = false;   // type & 128 (mania long note)
    int endTimeMs = 0;     // == timeMs for circles
    int column = 0;        // mania: floor(x * keyCount / 512), clamped
    // Split extras: holds endTime:normalSet:additionSet:index:volume:filename,
    // circles normalSet:...:filename; excess ':' folds into filename.
    QList<Token> samples;

    struct Key {
        int timeMs = 0;
        int column = 0;
        int occurrence = 0; // >0 only for duplicate (time,column) — warned
        auto operator<=>(const Key&) const = default;
    } key;

    // Sample fields with the hold endTime stripped, for cross-type comparison.
    QList<Token> samplesNoEnd() const;
};

struct CanonicalMap {
    int formatVersion = -1;
    int mode = 0;
    int keyCount = 0; // mania: round(CircleSize), >=1; 0 for other modes

    // KV in file order (Bookmarks and Tags are lifted out into typed lists).
    QList<QPair<QByteArray, Token>> general, editor, metadata, difficulty;
    QList<qint64> bookmarks; // sorted
    QList<QByteArray> tagList;

    std::optional<Token> backgroundFile, videoFile;
    QList<BreakPeriod> breaks;          // sorted by startMs
    QList<QByteArray> storyboardLines;  // opaque until the SB milestone; order kept
    QList<TimingPoint> timing;          // sorted by key
    QList<CanonicalNote> notes;         // sorted by key — chord order is canonical here

    const Token* kv(SectionId section, QByteArrayView key) const;
};

CanonicalMap canonicalize(const OsuDocument& doc, QList<ParseWarning>* warnings = nullptr);

} // namespace ovc::osu
