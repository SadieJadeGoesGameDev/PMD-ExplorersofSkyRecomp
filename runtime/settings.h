// Sky Recomp options shared between the frontend and the recompiled game code (Recomp Options menu, patches).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

enum { SADIE_ANY_STARTER, SADIE_TEAM_CONTROL, SADIE_GENTLE_HUNGER, SADIE_KEEP_ITEMS, SADIE_IQ_PER_LEVEL, SADIE_TEAM_EXP, SADIE_COUNT };
enum { FILTER_OFF, FILTER_XBRZ, FILTER_CRT, FILTER_SMOOTH, FILTER_GLOW, FILTER_LCD, FILTER_COUNT };
enum { DISPLAY_WINDOWED, DISPLAY_BORDERLESS, DISPLAY_FULLSCREEN, DISPLAY_COUNT };

typedef struct RecompSettings {
    int widescreen;           // 0/1
    int filter;               // FILTER_*
    int display;              // DISPLAY_*
    int resolution;           // index into the frontend's resolution list
    int save_slot;            // 1..3
    int sadie[SADIE_COUNT];   // 0/1 each, default on
    int dirty;                // set when changed from the in-game menu; frontend applies + saves
    int request_exit;         // Exit to Windows confirmed
    int request_slot;         // slot switch requested (0 = none)
    int autosave;             // 0/1, default on
} RecompSettings;

extern RecompSettings recomp_settings;

#ifdef __cplusplus
}
#endif
