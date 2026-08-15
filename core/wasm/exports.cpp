// C ABI surface for the WASM build. Callers pass raw .osu bytes and receive
// malloc'd JSON strings they must release with ovc_free.
#include <ovccore/canonical.h>
#include <ovccore/diff.h>
#include <ovccore/json.h>
#include <ovccore/merge.h>
#include <ovccore/parser.h>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

char* dup(const std::string& s)
{
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (out) std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

ovc::core::CanonicalMap canon(const char* p, int len)
{
    return ovc::core::canonicalize(ovc::core::parseOsu({p, size_t(len)}).doc);
}

} // namespace

extern "C" {

const char* ovc_version()
{
    return "ovc-core 0.2.0";
}

// serialize(parse(x)) == x — the lossless invariant, callable from tests.
int ovc_roundtrip_ok(const char* p, int len)
{
    const std::string_view in{p, size_t(len)};
    return ovc::core::serializeOsu(ovc::core::parseOsu(in).doc) == in ? 1 : 0;
}

char* ovc_diff_json(const char* a, int alen, const char* b, int blen)
{
    return dup(ovc::core::diffToJson(ovc::core::diffBeatmaps(canon(a, alen), canon(b, blen))));
}

char* ovc_map_json(const char* p, int len)
{
    return dup(ovc::core::mapToJson(canon(p, len)));
}

// 3-way merge. `resJson` is a JSON object of { "<conflict id>": "theirs" }
// entries (anything absent stays ours); pass "" or "{}" for ours-wins. Returns
// JSON { clean, wholeFileConflict, reason, conflicts[], merged }.
char* ovc_merge_json(const char* b, int bl, const char* o, int ol, const char* t, int tl,
                     const char* resJson, int rl)
{
    using namespace ovc::core;
    // Tiny hand parse of {"id":"theirs",...} — ids never contain '"' or '\'.
    ResolutionMap res;
    const std::string rs(resJson, resJson + (rl > 0 ? rl : 0));
    for (size_t i = 0; i < rs.size();) {
        const size_t k0 = rs.find('"', i);
        if (k0 == std::string::npos) break;
        const size_t k1 = rs.find('"', k0 + 1);
        if (k1 == std::string::npos) break;
        const size_t v0 = rs.find('"', k1 + 1);
        if (v0 == std::string::npos) break;
        const size_t v1 = rs.find('"', v0 + 1);
        if (v1 == std::string::npos) break;
        const std::string id = rs.substr(k0 + 1, k1 - k0 - 1);
        const std::string side = rs.substr(v0 + 1, v1 - v0 - 1);
        res[id] = side == "theirs" ? ResolveSide::Theirs : ResolveSide::Ours;
        i = v1 + 1;
    }
    const MergeResult r = merge3(canon(b, bl), canon(o, ol), canon(t, tl), res);

    auto esc = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else out += c;
        }
        return out;
    };

    std::string j = "{\"clean\":";
    j += r.clean() ? "true" : "false";
    j += ",\"wholeFileConflict\":";
    j += r.wholeFileConflict ? "true" : "false";
    j += ",\"reason\":\"" + esc(r.reason) + "\",\"conflicts\":[";
    for (size_t i = 0; i < r.conflicts.size(); ++i) {
        const Conflict& c = r.conflicts[i];
        if (i) j += ',';
        j += "{\"id\":\"" + esc(c.id) + "\",\"key\":\"" + esc(c.key) +
             "\",\"timeMs\":" + std::to_string(c.timeMs) +
             ",\"column\":" + std::to_string(c.column) + ",\"base\":\"" + esc(c.base) +
             "\",\"ours\":\"" + esc(c.ours) + "\",\"theirs\":\"" + esc(c.theirs) + "\"}";
    }
    j += "],\"merged\":\"";
    j += r.wholeFileConflict ? std::string() : esc(emitCanonical(r.merged));
    j += "\"}";
    return dup(j);
}

void ovc_free(char* p)
{
    std::free(p);
}

} // extern "C"
