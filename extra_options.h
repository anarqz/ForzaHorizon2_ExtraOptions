#ifndef FH2_EXTRA_OPTIONS_H
#define FH2_EXTRA_OPTIONS_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#define MOD_NAME "Forza Horizon 2 Extra Options"
#define MOD_VERSION "0.2.34+fh2online.16"
#define MOD_INI_NAME L"FH2_ExtraOptions.ini"
#define MOD_LOG_NAME L"FH2_ExtraOptions.log"
#define GAME_EXE_NAME L"forza_durango_release_final.exe"
#define PATH_CAPACITY 32768
#define GAME_FILE_SIZE 58204160u
#define GAME_IMAGE_SIZE 0x3a18000u
#define GAME_TIMESTAMP 0x565501edu
#define LANGUAGE_AUTO -1
#define LANGUAGE_COUNT 17
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

typedef enum {
    GROUP_DLC,
    GROUP_VIP,
    GROUP_INTRO,
    GROUP_MARKETPLACE,
    GROUP_LIVERY,
    GROUP_MSAA,
    GROUP_PAUSE_GARAGE,
    GROUP_TEXT_INIT,
    GROUP_TEXT_LOCK,
    GROUP_AUDIO_LANGUAGE,
    GROUP_TEXT_RELOAD,
    GROUP_ROAD,
    GROUP_ISLAND_CARS,
    GROUP_PARENTAL,
    GROUP_GAMERPIC,
    GROUP_PHOTO_CAMERA,
    GROUP_TREASURE_MAP,
    GROUP_AUTODRIVE,
    GROUP_SUN_SHADOWS
} PatchGroup;

typedef struct {
    uint32_t rva, offset, size;
    PatchGroup group;
    const char *name;
    const unsigned char *original, *standard, *supersampled;
    const unsigned char *const *accepted;
    uint32_t accepted_count;
} PatchDefinition;

typedef struct {
    int enabled;
    int unlock_dlc;
    int offline_vip;
    int skip_intro;
    int enable_marketplace;
    int enable_4k_liveries;
    int livery_msaa;
    int livery_supersampling;
    int garage_from_pause;
    int text_language;
    int audio_language;
    int snap_car_to_road;
    int unlock_storm_island_cars;
    int disable_parental_locks;
    int custom_gamerpic;
    int free_photo_camera;
    int unlock_treasure_map;
    int autodrive;
    int autodrive_cinematic;
    int autodrive_events;
    int disable_azimuth_save_checksums;
    int smooth_sun_shadows;
} ModOptions;
#endif
