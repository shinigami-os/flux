#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../include/alpine.h"
#include "../include/util.h"

int alpine_apk_url(const flux_config_t *config, const char *repo, const char *arch, const char *name, const char *version, char *out, size_t outlen) {
    snprintf(out, outlen, "%s/%s/%s/%s/%s-%s.apk", config->alpine_mirror_url, config->alpine_branch, repo, arch, name, version);
    return FLUX_ERR_NONE;
}

int alpine_apk_download(const flux_config_t *config, const char *repo, const char *arch, const char *name, const char *version, char *path_out, size_t path_outlen) {
    char url[FLUX_MAX_URL_LEN * 2];
    if (alpine_apk_url(config, repo, arch, name, version, url, sizeof(url)) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    system("mkdir -p /tmp/flux-build");
    snprintf(path_out, path_outlen, "/tmp/flux-build/%s-%s.apk", name, version);
    return flux_download(url, path_out);
}

// scans for gzip's magic bytes (1f 8b 08) directly in C - not shelled out, since BusyBox grep (what Kira actually
// runs) has no -b (byte offset) flag at all, unlike the GNU grep this was originally developed and tested against
static int find_magic_offsets(const char *path, long *candidates, int max_candidates, int *candidate_count) {
    *candidate_count = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return FLUX_ERR_GENERAL;

    unsigned char buf[8192];
    long pos = 0;
    size_t n = fread(buf, 1, sizeof(buf), f);

    while (n >= 3) {
        for (size_t i = 0; i + 2 < n; i++) {
            if (buf[i] == 0x1f && buf[i + 1] == 0x8b && buf[i + 2] == 0x08) {
                if (*candidate_count < max_candidates) candidates[(*candidate_count)++] = pos + (long)i;
            }
        }
        buf[0] = buf[n - 2];
        buf[1] = buf[n - 1];
        pos += (long)(n - 2);
        size_t got = fread(buf + 2, 1, sizeof(buf) - 2, f);
        if (got == 0) break;
        n = got + 2;
    }
    fclose(f);
    return FLUX_ERR_NONE;
}

// gzip -t needs an exact whole-member byte range (not monotonic in N), so each magic-byte candidate is confirmed with an exact-range gzip -t
int alpine_gzip_find_members(const char *path, long *offsets, int max_offsets, int *count) {
    *count = 0;

    long candidates[64];
    int candidate_count = 0;
    if (find_magic_offsets(path, candidates, 64, &candidate_count) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    if (candidate_count == 0 || candidates[0] != 0) return FLUX_ERR_SOURCE;

    offsets[(*count)++] = 0;
    for (int i = 1; i < candidate_count && *count < max_offsets; i++) {
        long start = offsets[*count - 1];
        long len = candidates[i] - start;
        char testcmd[600];
        snprintf(testcmd, sizeof(testcmd), "tail -c +%ld \"%s\" | head -c %ld | gzip -t 2>/dev/null", start + 1, path, len);
        if (system(testcmd) == 0) offsets[(*count)++] = candidates[i];
    }
    return FLUX_ERR_NONE;
}

int alpine_gzip_extract_range(const char *path, long start, long len, const char *out_path) {
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "tail -c +%ld \"%s\" | head -c %ld > \"%s\"", start + 1, path, len, out_path);
    return system(cmd) == 0 ? FLUX_ERR_NONE : FLUX_ERR_GENERAL;
}

int alpine_apk_split_members(const char *apk_path, char *sig_path_out, char *control_path_out, char *data_path_out, size_t path_outlen) {
    struct stat st;
    if (stat(apk_path, &st) != 0) return FLUX_ERR_SOURCE;

    long offsets[8];
    int count = 0;
    if (alpine_gzip_find_members(apk_path, offsets, 8, &count) != FLUX_ERR_NONE) return FLUX_ERR_SOURCE;
    if (count != 3) {
        flux_err(".apk does not have the expected 3 gzip members (found %d)", count);
        return FLUX_ERR_SOURCE;
    }

    long bounds[4] = { offsets[0], offsets[1], offsets[2], (long)st.st_size };
    char *out_paths[3] = { sig_path_out, control_path_out, data_path_out };
    const char *suffixes[3] = { "sig", "control", "data" };

    system("mkdir -p /tmp/flux-build");
    for (int i = 0; i < 3; i++) {
        snprintf(out_paths[i], path_outlen, "/tmp/flux-build/apk-%s.tar.gz", suffixes[i]);
        if (alpine_gzip_extract_range(apk_path, bounds[i], bounds[i + 1] - bounds[i], out_paths[i]) != FLUX_ERR_NONE)
            return FLUX_ERR_GENERAL;
    }
    return FLUX_ERR_NONE;
}

int alpine_apk_extract(const char *member_tar_gz_path, const char *destdir) {
    char cmd[700];
    // 2>/dev/null: tar warns about Alpine's own APK-TOOLS.checksum.SHA1 pax
    // header on every file, harmless noise unrelated to extraction success
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && mkdir -p \"%s\" && tar -xzf \"%s\" -C \"%s\" 2>/dev/null", destdir, destdir, member_tar_gz_path, destdir);
    return system(cmd) == 0 ? FLUX_ERR_NONE : FLUX_ERR_GENERAL;
}
