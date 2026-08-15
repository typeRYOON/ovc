# @ovc/core — WASM diff engine

The osu! beatmap parse / canonicalize / semantic-diff engine, compiled to WASM
from the same C++ that the ovc desktop app links natively. One implementation,
zero drift — the golden-corpus guarantees hold identically in both.

Vendor this folder into the website repo (e.g. `src/lib/ovc-core/`).

## Use

```ts
import { loadOvcCore } from "./ovc-core"; // index.mjs
const ovc = await loadOvcCore();

const diff = ovc.diff(beforeBytes, afterBytes); // Uint8Array in, BeatmapDiff out
const map  = ovc.map(afterBytes);               // timeline render source
ovc.roundtripOk(bytes);                         // lossless invariant check
```

Types are in `ovc-core.d.ts`. Everything runs client-side; no network, no server.

## Files

| file | what |
|---|---|
| `index.mjs` | loader + typed wrapper (`loadOvcCore`) — the entry point |
| `ovc-core.d.ts` | TypeScript contract for the diff/map JSON and the module surface |
| `ticks.ts` | beat-snap tick generation for the timeline grid (see below) |
| `dist/ovc-core.{mjs,wasm}` | Emscripten output (~152 KB wasm) |
| `demo.html` + `serve.py` | drop-two-files proof page (`python serve.py`) |
| `run-vectors.mjs` | node test: feeds the try_unite corpus through WASM |

## Timeline ticks

`ticks.ts` is a standalone TS reference (not WASM) that turns a map's `timing`
array into positioned, osu!-colored beat-snap ticks. It's a rendering
convention, so it lives in your language for your render loop:

```ts
import { buildTicks } from "./ticks";
const ticks = buildTicks(map.timing, viewFromMs, viewToMs, snapDivisor); // 1/4 = 4
```

## Rebuilding

Requires the emsdk at `E:\ryoon\tools\emsdk`. From the repo root run
`core\wasm\build.bat`, then `node core/wasm/run-vectors.mjs` to verify. The C++
sources live in `core/src`; only re-run when the engine changes.

## MIME note

ES modules and `.wasm` need correct MIME types. `python -m http.server` sends
`text/plain` for `.mjs` and browsers then refuse the module — use the bundled
`serve.py`, or configure your host (Cloudflare Pages serves these correctly).
