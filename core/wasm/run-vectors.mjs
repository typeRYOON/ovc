// Feeds the try_unite corpus through the WASM build and verifies the same
// guarantees the native core_tests assert. Run: node core/wasm/run-vectors.mjs
import { readFileSync, readdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { loadOvcCore } from "./index.mjs";

const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const corpus = join(root, "refs", "try_unite");

let failures = 0;
const check = (cond, label) => {
    if (!cond) {
        ++failures;
        console.error("FAIL", label);
    }
};

const ovc = await loadOvcCore();
console.log(ovc.version());

const osuFiles = readdirSync(corpus).filter((f) => f.endsWith(".osu"));
check(osuFiles.length === 2, "corpus has 2 .osu files");

const bytes = osuFiles.map((f) => new Uint8Array(readFileSync(join(corpus, f))));
for (let i = 0; i < osuFiles.length; ++i)
    check(ovc.roundtripOk(bytes[i]), `roundtrip ${osuFiles[i]}`);

const hs = osuFiles.findIndex((f) => f.includes("Hitsounds"));
const lb = osuFiles.findIndex((f) => f.includes("Lagrange"));
const diff = ovc.diff(bytes[hs], bytes[lb]);
check(diff.version === "Lagrange Blossom", "diff version field");
check(diff.empty === false, "diff non-empty");
check(diff.notes.length > 1000, `notes changes (${diff.notes.length})`);
check(diff.kv.some((c) => c.key === "OverallDifficulty" && c.after === "8"), "OD kv change");
check(diff.summary.length > 0, "summary present");

const map = ovc.map(bytes[lb]);
check(map.keyCount === 7, "keyCount 7");
check(map.notes.length === 3800, `note count (${map.notes.length})`);
check(map.audioFilename === "audio.mp3", "audio filename");
check(map.timing.length === 13, `timing count (${map.timing.length})`);

const self = ovc.diff(bytes[lb], bytes[lb]);
check(self.empty === true, "self-diff empty");

// 3-way merge: base = LB; ours edits one note's hitsound, theirs edits a
// different note — disjoint, so it merges clean.
const enc = new TextEncoder();
const lbText = new TextDecoder().decode(bytes[lb]);
const oursText = lbText.replace('36,192,1317,1,0,', '36,192,1317,1,8,'); // ours: add a clap hitsound
const theirsText = lbText.replace('OverallDifficulty:8', 'OverallDifficulty:6'); // theirs: OD
check(oursText !== lbText, "test vector actually changed a note");
const merged = ovc.merge(bytes[lb], enc.encode(oursText), enc.encode(theirsText));
check(merged.clean === true, `merge clean (conflicts: ${merged.conflicts.length})`);
check(merged.merged.includes('OverallDifficulty:6'), "merged has theirs' OD");
check(merged.merged.includes('36,192,1317,1,8,'), "merged has ours' hitsound");

// Conflicting merge: both change OD differently.
const conflict = ovc.merge(
    bytes[lb],
    enc.encode(lbText.replace('OverallDifficulty:8', 'OverallDifficulty:3')),
    enc.encode(lbText.replace('OverallDifficulty:8', 'OverallDifficulty:9')),
);
check(conflict.clean === false, "conflicting merge flagged");
check(conflict.conflicts.some((c) => c.key === 'OverallDifficulty'), "OD conflict reported");
check(conflict.conflicts.every((c) => typeof c.id === 'string' && c.id.length > 0), "conflicts carry ids");

// Resolve that conflict to theirs by id → merged now carries OD 9.
const odConflict = conflict.conflicts.find((c) => c.key === 'OverallDifficulty');
const resolved = ovc.merge(
    bytes[lb],
    enc.encode(lbText.replace('OverallDifficulty:8', 'OverallDifficulty:3')),
    enc.encode(lbText.replace('OverallDifficulty:8', 'OverallDifficulty:9')),
    { [odConflict.id]: 'theirs' },
);
check(resolved.merged.includes('OverallDifficulty:9'), "resolution took theirs");

console.log(failures === 0 ? "wasm vectors: all good" : `${failures} failures`);
process.exit(failures === 0 ? 0 : 1);
