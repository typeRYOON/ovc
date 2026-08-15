// Type contract for @ovc/core — the WASM diff engine shared by the desktop app
// and the ryoon.moe viewer. Numeric .osu values are emitted as their verbatim
// token strings (display values; nothing downstream does math on them).
// Fixed-point integer fields use a "…Milli" suffix (value × 1000).

// ─── Diff JSON (ovc.diff / GET /v1/diff → files[].semantic) ──────────────────

export type ChangeOp = "added" | "removed" | "modified";
export type NoteOp = ChangeOp | "moved";

/** One changed key inside a keyed row (a note, timing point, or KV pair). */
export interface FieldChange {
    key: string; // "type" | "endTime" | "hitSound" | "samples" | "x" | "y" | "curve" | "length" | "slides" | "edgeSounds" | "beatLength" | …
    before: string; // verbatim token ("" when the field was absent)
    after: string;
}

export interface KvChange {
    section: "General" | "Editor" | "Metadata" | "Difficulty";
    key: string;
    before: string; // "" ⇒ key was added
    after: string; // "" ⇒ key was removed
}

export interface ListChange {
    added: string[];
    removed: string[];
}

export interface BreakChange {
    op: ChangeOp;
    startMs: number;
    endMs: number;
    beforeEndMs?: number; // present only when op === "modified"
}

export interface EventsDiff {
    background: { before: string; after: string } | null;
    breaks: BreakChange[];
    sbAdded: number; // storyboard lines added (opaque count in v1)
    sbRemoved: number;
}

export interface TimingChange {
    op: ChangeOp;
    timeMs: number;
    uninherited: boolean; // true = red line (BPM), false = green (SV)
    bpmMilli: number; // BPM × 1000 (only meaningful when uninherited)
    svMilli: number; // slider-velocity multiplier × 1000 (green lines)
    kiai: boolean;
    fields: FieldChange[]; // populated only when op === "modified"
}

export interface NoteChange {
    op: NoteOp;
    timeMs: number;
    column: number; // mania column, 0-based
    fromColumn?: number; // present only when op === "moved"
    isHold: boolean;
    endTimeMs: number; // === timeMs for non-holds
    fields: FieldChange[]; // populated only when op === "modified"
}

export interface BeatmapDiff {
    version: string; // difficulty name (after side)
    summary: string; // "+10 -6 ~7 5 moved notes · OD 8->8.3"
    empty: boolean;
    modeChanged: boolean;
    keyCountChanged: boolean; // mania: CircleSize changed — notes[] is then empty
    keyCountBefore: number;
    keyCountAfter: number;
    affectedRange: [number, number] | null; // [minMs, maxMs] of timed changes
    kv: KvChange[];
    bookmarks: ListChange;
    tags: ListChange;
    events: EventsDiff;
    timing: TimingChange[];
    notes: NoteChange[]; // move-suppressed ghosts are already filtered out
}

// ─── Map JSON (ovc.map / GET /v1/map) — timeline render source ───────────────

export interface TimingPointLite {
    timeMs: number;
    uninherited: boolean;
    beatLen: string; // verbatim token; parseFloat for full precision
    meter: number; // beats per measure (defaults to 4)
    kiai: boolean;
}

/** [timeMs, column, endTimeMs] — endTimeMs === timeMs for a normal note. */
export type NoteTriple = [number, number, number];

export interface BeatmapMap {
    mode: number; // 0 std, 1 taiko, 2 catch, 3 mania
    keyCount: number; // mania column count (0 for other modes)
    version: string;
    audioFilename: string;
    backgroundFilename: string;
    bookmarks: number[]; // ms
    breaks: [number, number][]; // [startMs, endMs]
    timing: TimingPointLite[];
    notes: NoteTriple[];
}

// ─── Module surface ──────────────────────────────────────────────────────────

// ─── 3-way merge (ovc.merge) ─────────────────────────────────────────────────

export interface MergeConflict {
    key: string; // "m:ss.mmm col 3", a KV key name, "storyboard", …
    timeMs: number; // -1 when not time-anchored
    column: number; // -1 when not a note
    base: string; // rendered value ("" = absent on that side)
    ours: string;
    theirs: string;
}

export interface MergeResult {
    clean: boolean; // no conflicts and not a whole-file bail
    wholeFileConflict: boolean; // mode / key count differ — cannot auto-merge
    reason: string; // set when wholeFileConflict
    conflicts: MergeConflict[]; // ours won each; merged is still usable
    merged: string; // merged .osu text ("" when wholeFileConflict)
}

export interface OvcCore {
    version(): string;
    /** serialize(parse(x)) === x — the lossless round-trip invariant. */
    roundtripOk(bytes: Uint8Array): boolean;
    /** Semantic diff of two .osu byte buffers. */
    diff(before: Uint8Array, after: Uint8Array): BeatmapDiff;
    /** Canonical map payload for the timeline viewer. */
    map(bytes: Uint8Array): BeatmapMap;
    /** 3-way semantic merge; ours wins each conflict so `merged` is always usable. */
    merge(base: Uint8Array, ours: Uint8Array, theirs: Uint8Array): MergeResult;
}

export function loadOvcCore(): Promise<OvcCore>;
