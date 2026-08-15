// Beat-snap tick generation for the timeline, ported from the (removed) desktop
// renderer so the web viewer's grid matches osu!'s editor exactly. Pure TS: the
// coloring is a rendering convention, not diff logic, so it lives here rather
// than in the WASM core — the site owns its own render loop and calling WASM
// per frame for tick positions would be wasteful.
//
// Usage: feed it the `timing` array from ovc.map(bytes) plus the visible ms
// window and the snap divisor, get back positioned+colored ticks to draw.

import type { TimingPointLite } from "./ovc-core";

export type TickKind = "measure" | "beat" | "sub";

export interface Tick {
    ms: number;
    kind: TickKind;
    /** rgba() string, ready for canvas strokeStyle/fillStyle. */
    color: string;
}

// osu! editor colors. Measures/beats are white; sub-beats are colored by the
// lowest-terms denominator of their position within the beat.
const MEASURE = "rgba(255,255,255,0.50)";
const BEAT = "rgba(255,255,255,0.22)";
const HALF = "rgba(204,68,68,0.55)"; // 1/2 red
const QUARTER = "rgba(68,136,204,0.55)"; // 1/4 blue
const EIGHTH = "rgba(204,170,68,0.50)"; // 1/8 yellow
const SIXTEENTH = "rgba(102,102,102,0.45)"; // 1/16 grey
const THIRD = "rgba(170,102,204,0.55)"; // 1/3 purple
const SIXTH = "rgba(170,102,204,0.40)"; // 1/6
const TWELFTH = "rgba(170,102,204,0.28)"; // 1/12

function gcd(a: number, b: number): number {
    while (b) [a, b] = [b, a % b];
    return a;
}

// Sub-beat color by lowest-terms denominator, matching the desktop table.
function subColor(k: number, divisor: number): string {
    switch (divisor / gcd(k, divisor)) {
        case 2: return HALF;
        case 4: return QUARTER;
        case 8: return EIGHTH;
        case 3: return THIRD;
        case 6: return SIXTH;
        case 12: return TWELFTH;
        default: return SIXTEENTH;
    }
}

/**
 * Ticks visible in [fromMs, toMs], generated per uninherited timing section.
 * `divisor` is the beat-snap denominator (1, 2, 3, 4, 6, 8, 12, 16).
 * Returns ticks in ascending time order.
 */
export function buildTicks(
    timing: TimingPointLite[],
    fromMs: number,
    toMs: number,
    divisor: number,
): Tick[] {
    const ticks: Tick[] = [];
    const red = timing.filter((t) => t.uninherited);
    const d = Math.max(1, divisor);

    for (let i = 0; i < red.length; i++) {
        const tp = red[i];
        const beatLen = parseFloat(tp.beatLen);
        if (!(beatLen > 0)) continue;
        const meter = tp.meter > 0 ? tp.meter : 4;
        const start = tp.timeMs;
        // A red line's section ends at the next red line (or the view edge).
        const sectionEnd = i + 1 < red.length ? Math.min(toMs, red[i + 1].timeMs) : toMs;
        if (sectionEnd < fromMs || start > toMs) continue;

        const step = beatLen / d;
        const firstN = Math.max(0, Math.ceil((Math.max(start, fromMs) - start) / step - 1e-9));
        const lastN = Math.floor((Math.min(sectionEnd, toMs) - start) / step + 1e-9);
        for (let n = firstN; n <= lastN; n++) {
            const ms = start + n * step;
            const k = n % d;
            if (k === 0) {
                const beatIndex = n / d;
                ticks.push({
                    ms,
                    kind: beatIndex % meter === 0 ? "measure" : "beat",
                    color: beatIndex % meter === 0 ? MEASURE : BEAT,
                });
            } else {
                ticks.push({ ms, kind: "sub", color: subColor(k, d) });
            }
        }
    }
    return ticks;
}
