#pragma once
// Application preferences, persisted at %AppData%\InstantFileFinder\settings.json.
//
// This is a hand-written flat key/value reader-writer, not a general JSON library: all values
// are bool or integer, so a tiny purpose-built format is enough and keeps the project free of
// third-party dependencies. See SettingsService.cpp for the exact (minimal) grammar accepted.

#include <cstdint>
#include <string>

// What to do, without asking, the next time the app starts non-elevated. Set from the
// StartupElevationDialog's "remember this choice" checkbox; AlwaysAsk (the default) means show
// that dialog again next time.
enum class StartupElevationChoice : uint32_t {
    AlwaysAsk = 0,
    AlwaysElevate = 1,
    AlwaysContinueNormal = 2,
};

struct AppSettings {
    // Master switch: when false (the default), no index is ever built automatically and search
    // runs live against the filesystem instead (see LiveSearchManager). When true, startup
    // auto-indexes fixed NTFS drives (subject to the flag below) and search uses the index.
    bool enableIndexing = false;

    bool autoIndexFixedNtfsDrives = true;
    bool includeRemovableDrives = false;
    bool includeNetworkDrives = false;
    bool useFastNtfsUsnScan = true;
    // Off by default: reading $MFT directly is the fastest indexing method but usually needs
    // the process to be running elevated (see RawMftIndexer.h). Selecting "Raw MFT" as a
    // drive's scan method in Index Drives... has no effect unless this is also enabled.
    bool useRawMftScan = false;
    bool persistIndexCache = true;
    bool useIncrementalUsnUpdates = true;
    bool includeHiddenFiles = true;
    bool includeSystemFiles = false;
    bool avoidReparsePoints = true;
    // Off by default: sizing a folder means recursively walking every descendant file, which can
    // be slow for large trees. When enabled, this runs automatically (in the background) after
    // each search for every folder in the results, so folders can be sorted by real size
    // alongside files (see FolderSizeCalculator).
    bool computeFolderSizes = false;
    uint32_t maxDisplayedResults = 10000;
    uint32_t searchDebounceMs = 500;
    bool alwaysOnTop = false;

    // Warn once per drive per session when its index is older than this many days. 0 disables
    // the warning.
    uint32_t indexStalenessWarningDays = 7;

    StartupElevationChoice startupElevationChoice = StartupElevationChoice::AlwaysAsk;
};

class SettingsService {
public:
    // Loads settings from disk, returning defaults for any missing/unparsable field and for a
    // missing or corrupt file entirely.
    static AppSettings Load();

    // Best-effort save; returns false if the settings directory/file could not be written.
    static bool Save(const AppSettings& settings);

    static std::wstring SettingsFilePath();
};
