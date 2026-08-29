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

// gzip -t only accepts a byte range covering an exact whole number of
// complete members, with nothing partial trailing - it is NOT true that
// "valid at N implies valid at every N' > N up to the next boundary", so a
// monotonic-predicate binary search doesn't work here. Real member starts
// are found by scanning for gzip's magic bytes (1f 8b 08) and confirming
// each candidate with an exact-range gzip -t, which also filters out any
// coincidental magic-byte match inside a member's own compressed data.
static int find_member_offsets(const char *apk_path, long *offsets, int max_offsets, int *count) {
    *count = 0;

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "grep -abo $'\\x1f\\x8b\\x08' \"%s\"", apk_path);
    FILE *f = popen(cmd, "r");
    if (!f) return FLUX_ERR_GENERAL;

    long candidates[64];
    int candidate_count = 0;
    char line[64];
    while (candidate_count < 64 && fgets(line, sizeof(line), f)) {
        char *endptr;
        long off = strtol(line, &endptr, 10);
        if (endptr != line) candidates[candidate_count++] = off;
    }
    pclose(f);

    if (candidate_count == 0 || candidates[0] != 0) return FLUX_ERR_SOURCE;

    offsets[(*count)++] = 0;
    for (int i = 1; i < candidate_count && *count < max_offsets; i++) {
        long start = offsets[*count - 1];
        long len = candidates[i] - start;
        char testcmd[600];
        snprintf(testcmd, sizeof(testcmd), "tail -c +%ld \"%s\" | head -c %ld | gzip -t 2>/dev/null", start + 1, apk_path, len);
        if (system(testcmd) == 0) offsets[(*count)++] = candidates[i];
    }
    return FLUX_ERR_NONE;
}

int alpine_apk_split_members(const char *apk_path, char *sig_path_out, char *control_path_out, char *data_path_out, size_t path_outlen) {
    struct stat st;
    if (stat(apk_path, &st) != 0) return FLUX_ERR_SOURCE;

    long offsets[8];
    int count = 0;
    if (find_member_offsets(apk_path, offsets, 8, &count) != FLUX_ERR_NONE) return FLUX_ERR_SOURCE;
    if (count != 3) {
        flux_err(".apk does not have the expected 3 gzip members (found %d)", count);
        return FLUX_ERR_SOURCE;
    }

    long bounds[4] = { offsets[0], offsets[1], offsets[2], (long)st.st_size };
    char *out_paths[3] = { sig_path_out, control_path_out, data_path_out };
    const char *suffixes[3] = { "sig", "control", "data" };

    for (int i = 0; i < 3; i++) {
        snprintf(out_paths[i], path_outlen, "/tmp/flux-build/apk-%s.tar.gz", suffixes[i]);
        long start = bounds[i], len = bounds[i + 1] - bounds[i];
        char cmd[700];
        snprintf(cmd, sizeof(cmd), "tail -c +%ld \"%s\" | head -c %ld > \"%s\"", start + 1, apk_path, len, out_paths[i]);
        if (system(cmd) != 0) return FLUX_ERR_GENERAL;
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
