// Typed convenience wrapper over the C ABI. Usage:
//   import { loadOvcCore } from "./index.mjs";
//   const ovc = await loadOvcCore();
//   const diff = ovc.diff(beforeBytes, afterBytes); // parsed JSON object
import createOvcCore from "./dist/ovc-core.mjs";

export async function loadOvcCore() {
    const m = await createOvcCore();

    const withBytes = (bytes, fn) => {
        const ptr = m._malloc(bytes.length);
        m.HEAPU8.set(bytes, ptr);
        try {
            return fn(ptr, bytes.length);
        } finally {
            m._free(ptr);
        }
    };

    const takeJson = (ptr) => {
        const text = m.UTF8ToString(ptr);
        m._ovc_free(ptr);
        return JSON.parse(text);
    };

    return {
        version: () => m.UTF8ToString(m._ovc_version()),

        // serialize(parse(x)) === x — the lossless invariant.
        roundtripOk: (bytes) => withBytes(bytes, (p, n) => m._ovc_roundtrip_ok(p, n) === 1),

        // Semantic diff of two .osu byte buffers (Uint8Array) → JSON object.
        diff: (before, after) =>
            withBytes(before, (bp, bn) =>
                withBytes(after, (ap, an) => takeJson(m._ovc_diff_json(bp, bn, ap, an)))),

        // Canonical map payload for the timeline viewer.
        map: (bytes) => withBytes(bytes, (p, n) => takeJson(m._ovc_map_json(p, n))),

        // 3-way merge of three .osu byte buffers → { clean, conflicts, merged, … }.
        // `resolutions` is an optional { [conflictId]: "ours" | "theirs" } map.
        merge: (base, ours, theirs, resolutions = {}) => {
            const resBytes = new TextEncoder().encode(JSON.stringify(resolutions));
            return withBytes(base, (bp, bn) =>
                withBytes(ours, (op, on) =>
                    withBytes(theirs, (tp, tn) =>
                        withBytes(resBytes, (rp, rn) =>
                            takeJson(m._ovc_merge_json(bp, bn, op, on, tp, tn, rp, rn))))));
        },
    };
}
