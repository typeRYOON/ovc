@echo off
rem Builds core/wasm/dist/ovc-core.mjs + .wasm. Requires emsdk at E:\ryoon\tools\emsdk.
setlocal
set EMXX=E:\ryoon\tools\emsdk\upstream\emscripten\em++.exe
cd /d "%~dp0..\.."
if not exist core\wasm\dist mkdir core\wasm\dist
"%EMXX%" -O2 -std=c++20 -Icore/include ^
    core/src/token.cpp core/src/parser.cpp core/src/peek.cpp ^
    core/src/canonical.cpp core/src/diff.cpp core/src/json.cpp ^
    core/src/emit.cpp core/src/merge.cpp ^
    core/wasm/exports.cpp ^
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createOvcCore ^
    -sEXPORTED_FUNCTIONS=_ovc_diff_json,_ovc_map_json,_ovc_merge_json,_ovc_roundtrip_ok,_ovc_version,_ovc_free,_malloc,_free ^
    -sEXPORTED_RUNTIME_METHODS=HEAPU8,UTF8ToString ^
    -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web,node ^
    -o core/wasm/dist/ovc-core.mjs
exit /b %errorlevel%
