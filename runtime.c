#include "runtime.h"
#include "settings.h"
#include "logging.h"
#include "support.h"
#include "features.h"
#include "patch_engine.h"

#ifdef FH2_AZIMUTH
#include "adapters/azimuth_host.h"
#endif

int runtime_initialize(unsigned char *game_image, const wchar_t *executable_path,
                       const wchar_t *settings_directory) {
    ModOptions options;
    settings_defaults(&options);
    gamerpic_set_directory(settings_directory);
    log_reset();
    int initialized = settings_read(settings_directory, &options);
    if (initialized && options.enabled) {
        initialized = verify_game_file(executable_path);
    }
    if (initialized) {
        initialized = patch_engine_apply(game_image, &options);
    }
    log_start(settings_directory, &options);
#ifdef FH2_AZIMUTH
    if (initialized && options.enabled) {
        log_set_directory(settings_directory);
        azimuth_apply_livery_compatibility();
        azimuth_apply_save_compatibility(options.disable_azimuth_save_checksums);
        azimuth_apply_network(settings_directory);
        azimuth_apply_economy_probe(game_image);
    }
#endif
    if (initialized && options.enabled &&
        (options.garage_from_pause || options.text_language >= 0 || options.audio_language >= 0 ||
         options.custom_gamerpic || options.autodrive)) {
        log_set_directory(settings_directory);
        if (options.garage_from_pause) {
            log_append("GARAGE: XML parse hook installed; awaiting menu-flow resources.\r\n");
        }
        log_append(language_diagnostics_get());
        if (options.autodrive) {
            log_append("AUTODRIVE: experimental F2 GPS/AI hooks installed; cinematic delay is 3 "
                       "seconds.\r\n");
        }
        if (options.custom_gamerpic) {
            log_append("GAMERPIC: converter hook installed; Gamerpic.png is read from the settings "
                       "folder on the first local-player request.\r\n");
        }
    }
    return initialized;
}

#ifdef FH2_TWEAKS_TEST
__declspec(dllexport) int Fh2TestInitialize(void *base, const wchar_t *path,
                                            const wchar_t *directory) {
    return runtime_initialize(base, path, directory);
}

__declspec(dllexport) int Fh2TestVerifyFile(const wchar_t *path) {
    return verify_game_file(path);
}

__declspec(dllexport) int Fh2TestReadOptions(const wchar_t *directory, ModOptions *options) {
    return settings_read(directory, options);
}
#endif
