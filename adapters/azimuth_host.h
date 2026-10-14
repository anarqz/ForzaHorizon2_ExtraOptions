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
int azimuth_account_register(const wchar_t *name, wchar_t *message, size_t cap);
int azimuth_account_new_web_key(wchar_t *message, size_t cap);
int azimuth_account_add_friend(const wchar_t *gamertag, wchar_t *message, size_t cap);
void azimuth_account_set_online(int online);
void azimuth_account_restart(void);

/* azimuth_online_menu.c: Configurações > MULTIPLAYER. */
void azimuth_apply_online_menu(unsigned char *game_image);
#endif
