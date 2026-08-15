#include <ovccore/json.h>

namespace ovc::core {

namespace {

// Minimal append-style writer; the schema is small and fixed.
struct Json {
    std::string& out;

    void raw(std::string_view s) { out += s; }
    void str(std::string_view s)
    {
        out += '"';
        for (const char c : s) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (uint8_t(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else {
                    out += c;
                }
            }
        }
        out += '"';
    }
    void num(int64_t v) { out += std::to_string(v); }
    void boolean(bool v) { out += v ? "true" : "false"; }
};

const char* opName(ChangeOp op)
{
    switch (op) {
    case ChangeOp::Added: return "added";
    case ChangeOp::Removed: return "removed";
    default: return "modified";
    }
}

void writeFieldChanges(Json& j, const std::vector<FieldChange>& fields)
{
    j.raw("[");
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) j.raw(",");
        j.raw("{\"key\":");
        j.str(fields[i].key);
        j.raw(",\"before\":");
        j.str(fields[i].before.raw);
        j.raw(",\"after\":");
        j.str(fields[i].after.raw);
        j.raw("}");
    }
    j.raw("]");
}

void writeListDiff(Json& j, const ListDiff& l)
{
    j.raw("{\"added\":[");
    for (size_t i = 0; i < l.added.size(); ++i) {
        if (i) j.raw(",");
        j.str(l.added[i]);
    }
    j.raw("],\"removed\":[");
    for (size_t i = 0; i < l.removed.size(); ++i) {
        if (i) j.raw(",");
        j.str(l.removed[i]);
    }
    j.raw("]}");
}

} // namespace

std::string diffToJson(const BeatmapDiff& d)
{
    std::string out;
    out.reserve(4096);
    Json j{out};

    j.raw("{\"version\":");
    j.str(d.version);
    j.raw(",\"summary\":");
    j.str(d.summary());
    j.raw(",\"empty\":");
    j.boolean(d.empty());
    j.raw(",\"modeChanged\":");
    j.boolean(d.modeChanged);
    j.raw(",\"keyCountChanged\":");
    j.boolean(d.keyCountChanged);
    j.raw(",\"keyCountBefore\":");
    j.num(d.keyCountBefore);
    j.raw(",\"keyCountAfter\":");
    j.num(d.keyCountAfter);

    const auto range = d.affectedTimeRange();
    if (range.first >= 0) {
        j.raw(",\"affectedRange\":[");
        j.num(range.first);
        j.raw(",");
        j.num(range.second);
        j.raw("]");
    }
    else {
        j.raw(",\"affectedRange\":null");
    }

    j.raw(",\"kv\":[");
    bool first = true;
    for (const KvDiff& sec : d.kv) {
        for (const FieldChange& f : sec.changes) {
            if (!first) j.raw(",");
            first = false;
            j.raw("{\"section\":");
            j.str(sectionName(sec.section));
            j.raw(",\"key\":");
            j.str(f.key);
            j.raw(",\"before\":");
            j.str(f.before.raw);
            j.raw(",\"after\":");
            j.str(f.after.raw);
            j.raw("}");
        }
    }
    j.raw("]");

    j.raw(",\"bookmarks\":");
    writeListDiff(j, d.bookmarks);
    j.raw(",\"tags\":");
    writeListDiff(j, d.tags);

    j.raw(",\"events\":{\"background\":");
    if (d.events.background) {
        j.raw("{\"before\":");
        j.str(d.events.background->before.raw);
        j.raw(",\"after\":");
        j.str(d.events.background->after.raw);
        j.raw("}");
    }
    else {
        j.raw("null");
    }
    j.raw(",\"video\":");
    if (d.events.video) {
        j.raw("{\"before\":");
        j.str(d.events.video->before.raw);
        j.raw(",\"after\":");
        j.str(d.events.video->after.raw);
        j.raw("}");
    }
    else {
        j.raw("null");
    }
    j.raw(",\"breaks\":[");
    for (size_t i = 0; i < d.events.breaks.size(); ++i) {
        if (i) j.raw(",");
        const BreakChange& b = d.events.breaks[i];
        const BreakPeriod& show = b.op == ChangeOp::Removed ? b.before : b.after;
        j.raw("{\"op\":");
        j.str(opName(b.op));
        j.raw(",\"startMs\":");
        j.num(show.startMs);
        j.raw(",\"endMs\":");
        j.num(show.endMs);
        if (b.op == ChangeOp::Modified) {
            j.raw(",\"beforeEndMs\":");
            j.num(b.before.endMs);
        }
        j.raw("}");
    }
    j.raw("],\"sbAdded\":");
    j.num(d.events.sbLinesAdded);
    j.raw(",\"sbRemoved\":");
    j.num(d.events.sbLinesRemoved);
    j.raw("}");

    j.raw(",\"timing\":[");
    for (size_t i = 0; i < d.timing.size(); ++i) {
        if (i) j.raw(",");
        const TimingChange& t = d.timing[i];
        const TimingPoint& show = t.op == ChangeOp::Removed ? t.before : t.after;
        j.raw("{\"op\":");
        j.str(opName(t.op));
        j.raw(",\"timeMs\":");
        j.num(t.timeQ / 1000);
        j.raw(",\"uninherited\":");
        j.boolean(t.uninherited);
        // Milli-fixed-point: integers avoid cross-language float formatting drift.
        j.raw(",\"bpmMilli\":");
        j.num(int64_t(show.bpm() * 1000));
        j.raw(",\"svMilli\":");
        j.num(int64_t(show.sv() * 1000));
        j.raw(",\"kiai\":");
        j.boolean(show.kiai());
        j.raw(",\"fields\":");
        writeFieldChanges(j, t.fields);
        j.raw("}");
    }
    j.raw("]");

    j.raw(",\"notes\":[");
    bool firstNote = true;
    for (const NoteChange& n : d.notes) {
        if (n.moveSuppressed) continue;
        if (!firstNote) j.raw(",");
        firstNote = false;
        const CanonicalNote& show = n.op == ChangeOp::Removed ? n.before : n.after;
        j.raw("{\"op\":");
        j.str(n.movedFromColumn >= 0 ? "moved" : opName(n.op));
        j.raw(",\"timeMs\":");
        j.num(n.timeMs);
        j.raw(",\"column\":");
        j.num(n.column);
        if (n.movedFromColumn >= 0) {
            j.raw(",\"fromColumn\":");
            j.num(n.movedFromColumn);
        }
        j.raw(",\"isHold\":");
        j.boolean(show.isHold);
        j.raw(",\"endTimeMs\":");
        j.num(show.endTimeMs);
        j.raw(",\"fields\":");
        writeFieldChanges(j, n.fields);
        j.raw("}");
    }
    j.raw("]}");
    return out;
}

std::string mapToJson(const CanonicalMap& m)
{
    std::string out;
    out.reserve(m.notes.size() * 48 + 2048);
    Json j{out};

    j.raw("{\"mode\":");
    j.num(m.mode);
    j.raw(",\"keyCount\":");
    j.num(m.keyCount);

    const Token* version = m.kv(SectionId::Metadata, "Version");
    j.raw(",\"version\":");
    j.str(version ? version->raw : "");
    const Token* audio = m.kv(SectionId::General, "AudioFilename");
    j.raw(",\"audioFilename\":");
    j.str(audio ? audio->raw : "");
    j.raw(",\"backgroundFilename\":");
    j.str(m.backgroundFile ? m.backgroundFile->raw : "");

    j.raw(",\"bookmarks\":[");
    for (size_t i = 0; i < m.bookmarks.size(); ++i) {
        if (i) j.raw(",");
        j.num(m.bookmarks[i]);
    }
    j.raw("]");

    j.raw(",\"breaks\":[");
    for (size_t i = 0; i < m.breaks.size(); ++i) {
        if (i) j.raw(",");
        j.raw("[");
        j.num(m.breaks[i].startMs);
        j.raw(",");
        j.num(m.breaks[i].endMs);
        j.raw("]");
    }
    j.raw("]");

    j.raw(",\"timing\":[");
    for (size_t i = 0; i < m.timing.size(); ++i) {
        if (i) j.raw(",");
        const TimingPoint& t = m.timing[i];
        j.raw("{\"timeMs\":");
        j.num(t.key.timeQ / 1000);
        j.raw(",\"uninherited\":");
        j.boolean(t.uninherited);
        j.raw(",\"beatLen\":");
        j.str(t.field(1).raw); // verbatim: the site parses with full precision
        int meter = t.field(2).toInt();
        if (meter <= 0) meter = 4;
        j.raw(",\"meter\":");
        j.num(meter);
        j.raw(",\"kiai\":");
        j.boolean(t.kiai());
        j.raw("}");
    }
    j.raw("]");

    j.raw(",\"notes\":[");
    for (size_t i = 0; i < m.notes.size(); ++i) {
        if (i) j.raw(",");
        const CanonicalNote& n = m.notes[i];
        j.raw("[");
        j.num(n.timeMs);
        j.raw(",");
        j.num(n.column);
        j.raw(",");
        j.num(n.isHold ? n.endTimeMs : n.timeMs);
        j.raw("]");
    }
    j.raw("]}");
    return out;
}

} // namespace ovc::core
