cmake_minimum_required(VERSION 3.21)

project(ovc VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent Test)
find_package(libgit2 CONFIG REQUIRED)
qt_standard_project_setup()

# -- osu domain: lossless .osu model, parser, diff (Qt Core only) -------------
qt_add_library(ovc_osu STATIC
    src/osu/token.cpp
    src/osu/parser.cpp
    src/osu/serializer.cpp
    src/osu/peek.cpp
    src/osu/canonical.cpp
    src/osu/diff.cpp
    include/osu/token.h
    include/osu/document.h
    include/osu/parser.h
    include/osu/serializer.h
    include/osu/peek.h
    include/osu/canonical.h
    include/osu/diff.h
    include/utils/timefmt.h
)
target_include_directories(ovc_osu PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ovc_osu PUBLIC Qt6::Core)

# -- git store: libgit2 confined to this target's sources ---------------------
qt_add_library(ovc_git STATIC
    src/git/gitcheck.cpp
    src/git/paths.cpp
    src/git/shadowrepo.cpp
    src/git/mirror.cpp
    src/git/setdiff.cpp
    src/git/registry.cpp
    src/git/ops.cpp
    src/git/gitraii.h
    include/git/gitcheck.h
    include/git/paths.h
    include/git/shadowrepo.h
    include/git/mirror.h
    include/git/setdiff.h
    include/git/registry.h
    include/git/ops.h
)
target_include_directories(ovc_git PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ovc_git
    PUBLIC Qt6::Core ovc_osu
    PRIVATE libgit2::libgit2package
)

# -- watch: memory reading + songs watching + snapshot orchestration ----------
qt_add_library(ovc_watch STATIC
    src/watch/processhandle.cpp
    src/watch/memoryscan.cpp
    src/watch/stablereader.cpp
    src/watch/gamewatcher.cpp
    src/watch/songsbinder.cpp
    src/watch/trackingservice.cpp
    include/watch/processhandle.h
    include/watch/memoryscan.h
    include/watch/osustate.h
    include/watch/stablereader.h
    include/watch/gamewatcher.h
    include/watch/songsbinder.h
    include/watch/trackingservice.h
)
target_include_directories(ovc_watch PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ovc_watch PUBLIC Qt6::Core Qt6::Concurrent ovc_git)

# -- ui -----------------------------------------------------------------------
qt_add_library(ovc_ui STATIC
    src/ui/mainwindow.cpp
    include/ui/mainwindow.h
)
target_include_directories(ovc_ui PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ovc_ui PUBLIC Qt6::Gui Qt6::Widgets ovc_watch)

# -- executables --------------------------------------------------------------
qt_add_executable(ovc src/main.cpp)
target_link_libraries(ovc PRIVATE ovc_ui)

qt_add_executable(ovc-cli src/main_cli.cpp)
target_link_libraries(ovc-cli PRIVATE ovc_osu ovc_git ovc_watch)

if (WIN32)
    set_target_properties(ovc PROPERTIES WIN32_EXECUTABLE TRUE)
    find_program(WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
    if (WINDEPLOYQT)
        foreach(tgt ovc ovc-cli)
            add_custom_command(TARGET ${tgt} POST_BUILD
                COMMAND "${WINDEPLOYQT}" --no-translations "$<TARGET_FILE:${tgt}>")
        endforeach()
    endif()
endif()

enable_testing()
qt_add_executable(test_roundtrip tests/test_roundtrip.cpp)
target_link_libraries(test_roundtrip PRIVATE ovc_osu Qt6::Test)
target_compile_definitions(test_roundtrip PRIVATE OVC_CORPUS_DIR="${CMAKE_SOURCE_DIR}/refs")
add_test(NAME roundtrip COMMAND test_roundtrip)

qt_add_executable(test_diff tests/test_diff.cpp)
target_link_libraries(test_diff PRIVATE ovc_osu Qt6::Test)
target_compile_definitions(test_diff PRIVATE OVC_CORPUS_DIR="${CMAKE_SOURCE_DIR}/refs")
add_test(NAME diff COMMAND test_diff)

qt_add_executable(test_store tests/test_store.cpp)
target_link_libraries(test_store PRIVATE ovc_git Qt6::Test)
add_test(NAME store COMMAND test_store)

qt_add_executable(test_pipeline tests/test_pipeline.cpp)
target_link_libraries(test_pipeline PRIVATE ovc_watch Qt6::Test)
add_test(NAME pipeline COMMAND test_pipeline)

if (WIN32 AND WINDEPLOYQT)
    foreach(tgt test_roundtrip test_diff test_store test_pipeline)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND "${WINDEPLOYQT}" --no-translations "$<TARGET_FILE:${tgt}>")
    endforeach()
endif()
