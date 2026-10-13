#include "settings.h"
#include "features.h"
#include "support.h"

#define INI_MAX_SIZE 8192
#define SETTING(section, key, field, type) {section, key, key, offsetof(ModOptions, field), type}

const SettingDefinition setting_definitions[] = {
    SETTING("Global", "Enabled", enabled, SETTING_BOOLEAN),
    SETTING("Content", "UnlockDLC", unlock_dlc, SETTING_BOOLEAN),
    SETTING("Content", "OfflineVIP", offline_vip, SETTING_BOOLEAN),
    SETTING("Frontend", "SkipIntro", skip_intro, SETTING_BOOLEAN),
    SETTING("Frontend", "EnableMarketplace", enable_marketplace, SETTING_BOOLEAN),
    SETTING("Livery", "Enable4K", enable_4k_liveries, SETTING_BOOLEAN),
    SETTING("Livery", "IntermediateMSAA", livery_msaa, SETTING_MSAA),
    SETTING("Livery", "ExtraSupersampling", livery_supersampling, SETTING_BOOLEAN),
    SETTING("Frontend", "GetInCarFromPauseMenu", garage_from_pause, SETTING_BOOLEAN),
    SETTING("Frontend", "SnapToRoadOnCarChange", snap_car_to_road, SETTING_BOOLEAN),
    SETTING("Content", "UnlockStormIslandCars", unlock_storm_island_cars, SETTING_BOOLEAN),
    SETTING("Content", "UnlockTreasureMap", unlock_treasure_map, SETTING_BOOLEAN),
    SETTING("Compatibility", "DisableParentalLocks", disable_parental_locks, SETTING_BOOLEAN),
    SETTING("Compatibility", "DisableAzimuthSaveChecksums", disable_azimuth_save_checksums,
            SETTING_BOOLEAN),
    SETTING("Profile", "CustomGamerpic", custom_gamerpic, SETTING_BOOLEAN),
    SETTING("PhotoMode", "FreeCamera", free_photo_camera, SETTING_BOOLEAN),
    SETTING("Graphics", "SmoothSunShadows", smooth_sun_shadows, SETTING_BOOLEAN),
    {"Autodrive", "Enabled", "AutodriveEnabled", offsetof(ModOptions, autodrive), SETTING_BOOLEAN},
    {"Autodrive", "Cinematic", "AutodriveCinematic", offsetof(ModOptions, autodrive_cinematic),
     SETTING_BOOLEAN},
    {"Language", "Text", "TextLanguage", offsetof(ModOptions, text_language),
     SETTING_TEXT_LANGUAGE},
    {"Language", "Audio", "AudioLanguage", offsetof(ModOptions, audio_language),
     SETTING_AUDIO_LANGUAGE},
};

const size_t setting_count = ARRAY_COUNT(setting_definitions);
static char ini_buffer[INI_MAX_SIZE + 1];
static wchar_t ini_path[PATH_CAPACITY];

void settings_defaults(ModOptions *options) {
    *options = (ModOptions){
        .enabled = 1,
        .unlock_dlc = 1,
        .offline_vip = 1,
        .skip_intro = 1,
        .enable_marketplace = 1,
        .enable_4k_liveries = 1,
        .livery_msaa = 4,
        .livery_supersampling = 0,
        .garage_from_pause = 1,
        .text_language = LANGUAGE_AUTO,
        .audio_language = LANGUAGE_AUTO,
        .snap_car_to_road = 1,
        .unlock_storm_island_cars = 1,
        .disable_parental_locks = 1,
        .custom_gamerpic = 1,
        .free_photo_camera = 1,
        .unlock_treasure_map = 0,
        .autodrive = 0,
        .autodrive_cinematic = 1,
        .disable_azimuth_save_checksums = 1,
        .smooth_sun_shadows = 0,
    };
}

int settings_value(const ModOptions *options, const SettingDefinition *definition) {
    const unsigned char *fields = (const unsigned char *)options;
    return *(const int *)(fields + definition->field_offset);
}

static char *trim_whitespace(char *text) {
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    size_t length = text_length(text);
    while (length &&
           (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r')) {
        text[--length] = 0;
    }
    return text;
}

static int known_section(const char *name) {
    if (text_equal_ignore_case(name, "General") ||
        text_equal_ignore_case(name, "Network")) {
        return 1;
    }
    for (size_t index = 0; index < setting_count; ++index) {
        if (text_equal_ignore_case(name, setting_definitions[index].section)) {
            return 1;
        }
    }
    return 0;
}

static const SettingDefinition *find_setting(const char *section, const char *key) {
    if (text_equal_ignore_case(section, "General")) {
        section = "Global";
    }
    for (size_t index = 0; index < setting_count; ++index) {
        const SettingDefinition *definition = &setting_definitions[index];
        if (text_equal_ignore_case(section, definition->section) &&
            text_equal_ignore_case(key, definition->key)) {
            return definition;
        }
    }
    return NULL;
}

static int parse_value(const SettingDefinition *definition, const char *text, int *value) {
    if (definition->type == SETTING_TEXT_LANGUAGE || definition->type == SETTING_AUDIO_LANGUAGE) {
        return language_parse_setting(text, definition->type == SETTING_AUDIO_LANGUAGE, value);
    }
    if (definition->type == SETTING_MSAA) {
        if (text_equal_ignore_case(text, "2")) {
            *value = 2;
            return 1;
        }
        if (text_equal_ignore_case(text, "4")) {
            *value = 4;
            return 1;
        }
        return 0;
    }
    if (text_equal_ignore_case(text, "0") || text_equal_ignore_case(text, "1")) {
        *value = text[0] - '0';
        return 1;
    }
    return 0;
}

static int parse_ini(char *text, ModOptions *options) {
    char section[32] = {0};
    if ((unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb &&
        (unsigned char)text[2] == 0xbf) {
        text += 3;
    }

    while (*text) {
        char *line = text;
        while (*text && *text != '\n') {
            ++text;
        }
        if (*text) {
            *text++ = 0;
        }
        for (char *cursor = line; *cursor; ++cursor) {
            if (*cursor == ';' || *cursor == '#') {
                *cursor = 0;
                break;
            }
        }
        line = trim_whitespace(line);
        if (!*line) {
            continue;
        }

        if (*line == '[') {
            char *closing = line;
            while (*closing && *closing != ']') {
                ++closing;
            }
            if (*closing != ']' || *trim_whitespace(closing + 1)) {
                goto invalid;
            }
            *closing = 0;
            char *name = trim_whitespace(line + 1);
            size_t length = text_length(name);
            if (length >= sizeof(section) || !known_section(name)) {
                goto invalid;
            }
            memory_copy(section, name, length + 1);
            continue;
        }

        char *separator = line;
        while (*separator && *separator != '=') {
            ++separator;
        }
        if (!*separator) {
            goto invalid;
        }
        *separator = 0;
        char *key = trim_whitespace(line);
        char *value = trim_whitespace(separator + 1);

        if (text_equal_ignore_case(section, "Frontend") &&
            text_equal_ignore_case(key, "HideOnlineTabs")) {
            if (!text_equal_ignore_case(value, "0") && !text_equal_ignore_case(value, "1")) {
                goto invalid;
            }
            continue;
        }

        /* FH2Online: [Network] is read by adapters/azimuth_net.c. */
        if (text_equal_ignore_case(section, "Network")) {
            if (text_equal_ignore_case(key, "Mode")) {
                if (!text_equal_ignore_case(value, "Off") &&
                    !text_equal_ignore_case(value, "Capture") &&
                    !text_equal_ignore_case(value, "Proxy")) {
                    goto invalid;
                }
                continue;
            }
            if (text_equal_ignore_case(key, "Server") && *value) {
                continue;
            }
            goto invalid;
        }

        const SettingDefinition *definition = find_setting(section, key);
        int parsed;
        if (!definition || !parse_value(definition, value, &parsed)) {
            goto invalid;
        }
        int *field = (int *)((unsigned char *)options + definition->field_offset);
        *field = parsed;
    }
    return 1;

invalid:
    status_set(
        "REFUSED: invalid INI section, option, or value. See the supplied FH2_ExtraOptions.ini.");
    return 0;
}

int settings_read(const wchar_t *directory, ModOptions *options) {
    settings_defaults(options);
    if (!path_join(ini_path, directory, MOD_INI_NAME)) {
        status_set("REFUSED: INI path is too long.");
        return 0;
    }
    HANDLE file = CreateFileW(ini_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            return 1;
        }
        status_set("REFUSED: cannot read FH2_ExtraOptions.ini.");
        return 0;
    }
    LARGE_INTEGER size;
    DWORD bytes_read;
    if (!GetFileSizeEx(file, &size) || size.QuadPart > INI_MAX_SIZE ||
        !ReadFile(file, ini_buffer, (DWORD)size.QuadPart, &bytes_read, NULL) ||
        bytes_read != size.QuadPart) {
        CloseHandle(file);
        status_set("REFUSED: INI read failed or file exceeds 8192 bytes.");
        return 0;
    }
    CloseHandle(file);
    ini_buffer[bytes_read] = 0;
    for (unsigned index = 0; index < bytes_read; ++index) {
        if (!ini_buffer[index]) {
            status_set("REFUSED: INI must use UTF-8 or ASCII, not UTF-16.");
            return 0;
        }
    }
    return parse_ini(ini_buffer, options);
}
