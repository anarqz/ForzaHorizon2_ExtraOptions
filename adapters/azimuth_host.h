#ifndef FH2_AZIMUTH_HOST_H
#define FH2_AZIMUTH_HOST_H
#include "extra_options.h"

typedef struct {
    unsigned offset, size;
    const unsigned char *bytes;
} HostCodePatch;

typedef struct {
    unsigned rva, size;
    const unsigned char *original_hash, *patched_hash;
    const HostCodePatch *patches;
    unsigned patch_count;
} HostPatchSet;

typedef enum { AZIMUTH_UNKNOWN, AZIMUTH_096, AZIMUTH_097 } AzimuthBuild;

AzimuthBuild azimuth_host_build(const unsigned char *base);

/* 1 = applied, 2 = already applied, 0 = unknown build, -1 = write failed. */
int azimuth_patch_host_code(unsigned char *base, const HostPatchSet *set);
int azimuth_patch_host_codes(unsigned char *base, const HostPatchSet *sets, unsigned count);
void azimuth_apply_livery_compatibility(void);
void azimuth_apply_save_compatibility(int disable_checksums);
void azimuth_apply_network(const wchar_t *directory);
void azimuth_apply_economy_probe(unsigned char *game_image);
void azimuth_apply_pad(void);

/* azimuth_net.c: the configured server, for the account module. */
const wchar_t *azimuth_net_server_name(void);
int azimuth_net_proxy_enabled(void);
DWORD azimuth_net_call(const wchar_t *method, const wchar_t *path, const wchar_t *extra_header,
                       const char *body, char *out, size_t capacity);
DWORD azimuth_net_request(const wchar_t *method, const wchar_t *path, const wchar_t *headers,
                          const wchar_t *content_type, const void *body, size_t body_size, void *out,
                          size_t capacity, size_t *received);
int azimuth_net_ping(int timeout_ms);
/* azimuth_cloud.c: the online profile's save, kept on the server. */
void azimuth_cloud_start(const wchar_t *bin, const wchar_t *xuid, const wchar_t *token, const wchar_t *ini,
                         const wchar_t *section);

/* azimuth_account.c: the player's FH2Online account on that server. */
void azimuth_apply_account(const wchar_t *directory);
const wchar_t *azimuth_account_token(void);
/* -1 unavailable, 0 no account, 1 account kept but offline profile, 2 playing online */
int azimuth_account_state(void);
const wchar_t *azimuth_account_gamertag(void);
const wchar_t *azimuth_account_web_key(void);
const wchar_t *azimuth_account_discriminator(void);
const wchar_t *azimuth_account_server(void);
int azimuth_account_valid_name(const wchar_t *name);
int azimuth_account_register(const wchar_t *name, int starter_car, wchar_t *message, size_t cap);
int azimuth_account_new_web_key(wchar_t *message, size_t cap);
int azimuth_account_add_friend(const wchar_t *gamertag, wchar_t *message, size_t cap);
void azimuth_account_set_online(int online);
void azimuth_account_restart(void);
int azimuth_account_log_in(const wchar_t *gamertag, const wchar_t *web_key, wchar_t *message, size_t cap);
const wchar_t *azimuth_account_notice(void);
void azimuth_account_test(wchar_t *out, size_t cap);
int azimuth_account_banner(char *out, size_t cap, const char **color);

/* azimuth_online_menu.c: Settings > INFINITY. */
void azimuth_apply_online_menu(unsigned char *game_image);
/* One tile of the INFINITY screen: heading (UTF-8, NULL = no tile), image or icon under
   Textures/Anthem/, photo = dark image tile with light text. */
typedef struct {
    const char *heading;
    const char *icon;
    int photo;
} InfinityTile;
/* The 7 tiles for the profile's current state (azimuth_online_menu.c). */
const InfinityTile *azimuth_infinity_tiles(void);
/* azimuth_ui.c: the CREDITS + INFINITY tiles in Settings, the INFINITY screen and the
   pause menu's status line. */
void azimuth_apply_ui(unsigned char *game_image);
int azimuth_ui_open_infinity(void *page);
#endif
