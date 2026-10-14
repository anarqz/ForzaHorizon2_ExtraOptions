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
#endif
