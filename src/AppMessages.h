#pragma once
// Custom window messages posted from background worker threads to the main window.
// Worker threads must never touch UI controls directly; they PostMessage here instead and
// MainWindow handles the message on the UI thread.

#include <windows.h>

// wParam = drive letter (WCHAR), lParam = unused. Sent when a drive's DriveIndexStatus changes
// (indexing started/finished, error recorded, item count updated).
constexpr UINT WM_APP_DRIVE_STATUS_CHANGED = WM_APP + 1;

// wParam = drive letter (WCHAR), lParam = items processed so far for that drive.
constexpr UINT WM_APP_INDEXING_PROGRESS = WM_APP + 2;

// wParam/lParam unused. Sent once when all requested drives have finished (or were cancelled).
constexpr UINT WM_APP_INDEXING_COMPLETE = WM_APP + 3;

// lParam = EnrichedResult* (heap-allocated by MetadataEnricher, freed by MainWindow's handler).
constexpr UINT WM_APP_METADATA_ENRICHED = WM_APP + 4;

// lParam = generation counter of the live search that produced this update (see
// LiveSearchManager). Sent periodically while a non-indexed search is scanning the filesystem.
constexpr UINT WM_APP_LIVE_SEARCH_PROGRESS = WM_APP + 5;

// Sent once when a live search finishes (or is superseded and the newest one finishes).
constexpr UINT WM_APP_LIVE_SEARCH_COMPLETE = WM_APP + 6;

// lParam = FolderSizeCalculator::ComputedResult* (heap-allocated by FolderSizeCalculator, freed
// by MainWindow's handler).
constexpr UINT WM_APP_FOLDER_SIZE_COMPUTED = WM_APP + 7;

// lParam = IndexedSearchWorker::CompletedSearch* (heap-allocated by IndexedSearchWorker, freed by
// MainWindow's handler). Sent once when an indexed search finishes (or is superseded and the
// newest one finishes) - see IndexedSearchWorker for why indexed search needs a background thread
// at all.
constexpr UINT WM_APP_INDEXED_SEARCH_COMPLETE = WM_APP + 8;
