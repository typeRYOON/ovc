# ovc-core: dependency-free engine (parse/canonicalize/diff/json). Builds
# natively for the desktop app and via Emscripten for the web viewer.
add_library(ovc_core STATIC
    src/token.cpp
    src/parser.cpp
    src/peek.cpp
    src/canonical.cpp
    src/diff.cpp
    src/json.cpp
    src/emit.cpp
    src/merge.cpp
    include/ovccore/token.h
    include/ovccore/document.h
    include/ovccore/parser.h
    include/ovccore/peek.h
    include/ovccore/canonical.h
    include/ovccore/diff.h
    include/ovccore/json.h
    include/ovccore/merge.h
    include/ovccore/timefmt.h
)
target_include_directories(ovc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(ovc_core PUBLIC cxx_std_20)
if (MSVC)
    target_compile_options(ovc_core PUBLIC /utf-8) # "·" in summaries; source is UTF-8
endif()

if (NOT EMSCRIPTEN)
    add_executable(core_tests tests/core_tests.cpp)
    target_link_libraries(core_tests PRIVATE ovc_core)
    target_compile_definitions(core_tests PRIVATE OVC_CORPUS_DIR="${CMAKE_SOURCE_DIR}/refs")
    add_test(NAME core COMMAND core_tests)
endif()
