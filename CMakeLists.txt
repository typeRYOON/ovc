cmake_minimum_required(VERSION 3.21)

project(ovc VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent Test HttpServer WebSockets)
find_package(libgit2 CONFIG REQUIRED)
enable_testing()
qt_standard_project_setup()

# -- engine: Qt-free core, shared with the web viewer (see core/) -------------
add_subdirectory(core)

# -- third party (C, warnings quieted) ----------------------------------------
add_library(ovc_thirdparty STATIC third_party/miniz/miniz.c)
target_include_directories(ovc_thirdparty PUBLIC third_party)
target_compile_definitions(ovc_thirdparty PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME)
set_target_properties(ovc_thirdparty PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# -- git store: libgit2 confined to this target's sources ---------------------
qt_add_library(ovc_git STATIC
    src/git/gitcheck.cpp
    src/git/paths.cpp
    src/git/shadowrepo.cpp
    src/git/mirror.cpp
    src/git/setdiff.cpp
    src/git/registry.cpp
    src/git/ops.cpp
    src/git/bundle.cpp
    src/git/mergesessions.cpp
    src/utils/config.cpp
    src/git/gitraii.h
    include/git/bundle.h
    include/git/mergesessions.h
    include/utils/config.h
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
    PUBLIC Qt6::Core ovc_core
    PRIVATE libgit2::libgit2package ovc_thirdparty
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

# -- serve: loopback API for the web viewer -----------------------------------
qt_add_library(ovc_serve STATIC
    src/serve/localserver.cpp
    include/serve/localserver.h
)
target_include_directories(ovc_serve PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ovc_serve PUBLIC Qt6::HttpServer Qt6::WebSockets ovc_watch)

# -- ui -----------------------------------------------------------------------
qt_add_library(ovc_ui STATIC
    src/chrome/titlebar.cpp
    src/chrome/windowchrome.cpp
    src/ui/mainwindow.cpp
    src/ui/mapsetmodel.cpp
    src/ui/snapshotmodel.cpp
    src/ui/updatechecker.cpp
    include/chrome/framelesschrome.h
    include/chrome/titlebar.h
    include/chrome/windowchrome.h
    include/ui/mainwindow.h
    include/ui/mapsetmodel.h
    include/ui/snapshotmodel.h
    include/ui/updatechecker.h
)
target_include_directories(ovc_ui PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ovc_ui PUBLIC Qt6::Gui Qt6::Widgets Qt6::Network ovc_serve)
# Bake the project version in so the update checker can compare against it.
target_compile_definitions(ovc_ui PRIVATE OVC_VERSION="${PROJECT_VERSION}")

# -- executables --------------------------------------------------------------
set(ovc_app_sources src/main.cpp resources/resources.qrc)
if (WIN32)
    # Embed the .exe icon (Explorer/taskbar). configure_file bakes an absolute
    # path into the .rc so rc.exe finds the .ico regardless of build cwd.
    set(OVC_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app.ico")
    configure_file(resources/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/ovc_app.rc" @ONLY)
    list(APPEND ovc_app_sources "${CMAKE_CURRENT_BINARY_DIR}/ovc_app.rc")
endif()
qt_add_executable(ovc ${ovc_app_sources})
target_link_libraries(ovc PRIVATE ovc_ui ovc_serve)

qt_add_executable(ovc-cli src/main_cli.cpp)
target_link_libraries(ovc-cli PRIVATE ovc_core ovc_git ovc_watch ovc_serve)

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

qt_add_executable(test_store tests/test_store.cpp)
target_link_libraries(test_store PRIVATE ovc_git Qt6::Test)
add_test(NAME store COMMAND test_store)

qt_add_executable(test_pipeline tests/test_pipeline.cpp)
target_link_libraries(test_pipeline PRIVATE ovc_watch Qt6::Test)
add_test(NAME pipeline COMMAND test_pipeline)

qt_add_executable(test_bundle tests/test_bundle.cpp)
target_link_libraries(test_bundle PRIVATE ovc_git Qt6::Test)
add_test(NAME bundle COMMAND test_bundle)

qt_add_executable(test_server tests/test_server.cpp)
target_link_libraries(test_server PRIVATE ovc_serve Qt6::Test Qt6::Network)
add_test(NAME server COMMAND test_server)

if (WIN32 AND WINDEPLOYQT)
    foreach(tgt test_store test_pipeline test_bundle test_server)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND "${WINDEPLOYQT}" --no-translations "$<TARGET_FILE:${tgt}>")
    endforeach()
endif()
