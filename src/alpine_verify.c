#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "../include/alpine.h"
#include "../include/util.h"

int alpine_verify_control(const char *control_tar_gz_path, const char *sig_tar_gz_path, const char *keys_dir) {
    const char *sig_extract_dir = "/tmp/flux-build/apk-sig-extract";
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && mkdir -p \"%s\" && tar -xzf \"%s\" -C \"%s\" 2>/dev/null",
             sig_extract_dir, sig_extract_dir, sig_tar_gz_path, sig_extract_dir);
    if (system(cmd) != 0) return FLUX_ERR_CACHE;

    DIR *d = opendir(sig_extract_dir);
    if (!d) return FLUX_ERR_CACHE;

    char sig_filename[256] = {0};
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, ".SIGN.RSA.", 10) == 0) {
            strncpy(sig_filename, e->d_name, sizeof(sig_filename) - 1);
            break;
        }
    }
    closedir(d);

    if (sig_filename[0] == '\0') {
        flux_err(".apk signature member has no .SIGN.RSA.* entry");
        return FLUX_ERR_CACHE;
    }

    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/%s", sig_extract_dir, sig_filename);

    char key_path[512];
    snprintf(key_path, sizeof(key_path), "%s/%s", keys_dir, sig_filename + 10); // skip ".SIGN.RSA."

    struct stat st;
    if (stat(key_path, &st) != 0) {
        flux_err("unknown Alpine signing key: %s", sig_filename + 10);
        return FLUX_ERR_CACHE;
    }

    char verify_cmd[2048];
    snprintf(verify_cmd, sizeof(verify_cmd), "openssl dgst -sha1 -verify \"%s\" -signature \"%s\" \"%s\" >/dev/null 2>&1",
             key_path, sig_path, control_tar_gz_path);
    int ok = (system(verify_cmd) == 0);

    char rmcmd[300];
    snprintf(rmcmd, sizeof(rmcmd), "rm -rf \"%s\"", sig_extract_dir);
    system(rmcmd);

    if (!ok) flux_err("signature verification failed");
    return ok ? FLUX_ERR_NONE : FLUX_ERR_CACHE;
}
