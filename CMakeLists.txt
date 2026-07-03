cmake_minimum_required(VERSION 3.20)
project(InstantFileFinder LANGUAGES CXX)

if (NOT WIN32)
    message(FATAL_ERROR "InstantFileFinder targets Windows (MSVC) only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Target Windows 10 / 11 APIs (needed for USN_RECORD_V3, SetProcessDpiAwarenessContext, etc.).
add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000000)
add_compile_definitions(UNICODE _UNICODE)
# NOMINMAX: keep windows.h from defining min()/max() macros that clash with std::min/std::max.
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)

set(SOURCES
    src/main.cpp
    src/App.cpp
    src/MainWindow.cpp
    src/AboutDialog.cpp
    src/PreferencesDialog.cpp
    src/IndexDrivesDialog.cpp
    src/SearchOptionsDialog.cpp
    src/StartupElevationDialog.cpp
    src/DriveEnumerator.cpp
    src/IndexManager.cpp
    src/DpiUtil.cpp
    src/NtfsUsnIndexer.cpp
    src/RawMftIndexer.cpp
    src/RecursiveFileScanner.cpp
    src/UsnIncrementalUpdater.cpp
    src/LiveSearchManager.cpp
    src/SearchEngine.cpp
    src/QueryParser.cpp
    src/PathReconstructor.cpp
    src/MetadataEnricher.cpp
    src/SettingsService.cpp
    src/Clipboard.cpp
    src/ShellHelpers.cpp
    src/StringUtil.cpp
    src/FileTimeUtil.cpp
    src/FormatUtil.cpp
)

set(HEADERS
    src/App.h
    src/MainWindow.h
    src/AboutDialog.h
    src/PreferencesDialog.h
    src/IndexDrivesDialog.h
    src/SearchOptionsDialog.h
    src/StartupElevationDialog.h
    src/DriveEnumerator.h
    src/IndexManager.h
    src/DpiUtil.h
    src/NtfsUsnIndexer.h
    src/RawMftIndexer.h
    src/RecursiveFileScanner.h
    src/UsnIncrementalUpdater.h
    src/LiveSearchManager.h
    src/SearchEngine.h
    src/QueryParser.h
    src/PathReconstructor.h
    src/MetadataEnricher.h
    src/SettingsService.h
    src/Clipboard.h
    src/ShellHelpers.h
    src/StringUtil.h
    src/FileTimeUtil.h
    src/FormatUtil.h
    src/Handle.h
    src/IndexTypes.h
    src/AppMessages.h
    src/ModalLoop.h
    src/Resource.h
)

add_executable(InstantFileFinder WIN32 ${SOURCES} ${HEADERS} resources/InstantFileFinder.rc)

set_target_properties(InstantFileFinder PROPERTIES
    OUTPUT_NAME "InstantFileFinder"
)

target_include_directories(InstantFileFinder PRIVATE src)

if (MSVC)
    # /utf-8 forces MSVC to treat source files as UTF-8 regardless of whether they have a BOM
    # and regardless of the build machine's ANSI code page. Without it, any non-ASCII byte in a
    # string literal (even something as simple as an em dash) is decoded using the *build
    # machine's* locale, so the exact same source can compile to different (sometimes garbled)
    # wide-character values on different machines. Source files should still stick to plain
    # ASCII in string literals as the primary defense; this is the backstop.
    target_compile_options(InstantFileFinder PRIVATE /W4 /permissive- /utf-8)
    # We embed our own manifest via resources/InstantFileFinder.rc (RT_MANIFEST id 1); disable
    # the linker's automatic manifest generation so it doesn't collide with that resource.
    target_link_options(InstantFileFinder PRIVATE /MANIFEST:NO)
endif()

target_link_libraries(InstantFileFinder PRIVATE
    user32
    gdi32
    comctl32
    shell32
    shlwapi
    advapi32
)
