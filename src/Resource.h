#pragma once
// Menu and control identifiers for the main window. The menu is built entirely in code with
// CreateMenu/AppendMenuW (no .rc menu template), so these are plain constants rather than
// resource-compiler symbols.

// ---- Menu commands ----
#define ID_FILE_OPEN_SELECTED        1001
#define ID_FILE_OPEN_FOLDER          1002
#define ID_FILE_COPY_PATH            1003
#define ID_FILE_COPY_RESULTS         1004
#define ID_FILE_EXIT                 1005

#define ID_INDEX_DRIVES              1010
#define ID_INDEX_REFRESH             1011
#define ID_INDEX_REBUILD             1012
#define ID_INDEX_CANCEL              1013
#define ID_INDEX_CLEAR_CACHE         1014
#define ID_INDEX_VIEW_DETAILS        1015
#define ID_INDEX_RUN_AS_ADMIN        1016

#define ID_SEARCH_CLEAR              1020
#define ID_SEARCH_OPTIONS            1021

#define ID_OPTIONS_PREFERENCES       1030
#define ID_OPTIONS_ALWAYS_ON_TOP     1031

#define ID_HELP_ABOUT                1040

// ---- Results list context menu (Open/Open Folder/Copy Path reuse the File menu IDs above) ----
#define ID_CONTEXT_DELETE            1050

// ---- Controls ----
#define IDC_EDIT_SEARCH               2001
#define IDC_COMBO_MODE                2002
#define IDC_COMBO_DRIVE               2003
#define IDC_COMBO_TYPE                2004
#define IDC_BUTTON_SEARCH             2005
#define IDC_BUTTON_CLEAR              2006
#define IDC_BUTTON_REFRESH            2007
#define IDC_LISTVIEW_RESULTS          2008
#define IDC_STATUSBAR                 2009

// ---- Timers ----
#define IDT_SEARCH_DEBOUNCE           3001
#define IDT_DRIVE_POLL                3002
#define IDT_ENRICH_REPAINT            3003
