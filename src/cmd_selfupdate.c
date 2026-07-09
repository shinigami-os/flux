#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../include/flux.h"
#include "../include/util.h"

static const char *strip_v(const char *tag) {
    return (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
}

static void current_binary_path(char *out, size_t outlen) {
    ssize_t n = readlink("/proc/self/exe", out, outlen - 1);
    if (n <= 0) {
        snprintf(out, outlen, "/usr/bin/flux");
        return;
    }
    out[n] = '\0';
}

int flux_self_update(int argc, char **argv, const char *usage) {
    (void)argc;
    (void)argv;
    (void)usage;

    printf("[flux] checking for a newer release...\n");
    char tag[64];
    int fetch_err = flux_fetch_latest_git_tag(FLUX_REPO_URL, tag, sizeof(tag));
    if (fetch_err == FLUX_ERR_NOT_FOUND) {
        printf("[flux] no release tags found at %s\n", FLUX_REPO_URL);
        return FLUX_ERR_NOT_FOUND;
    }
    if (fetch_err != FLUX_ERR_NONE) {
        fprintf(stderr, "flux: could not reach %s to check for updates\n", FLUX_REPO_URL);
        return FLUX_ERR_NETWORK;
    }

    const char *version = strip_v(tag);
    if (strcmp(version, FLUX_VERSION) == 0) {
        printf("[flux] already up to date (%s)\n", FLUX_VERSION);
        return FLUX_ERR_NONE;
    }

    if (system("command -v gcc >/dev/null 2>&1") != 0 ||
        system("command -v make >/dev/null 2>&1") != 0) {
        fprintf(stderr, "flux: gcc/make not found, can't build flux from source\n");
        fprintf(stderr, "hint: run 'flux install build-essential' first\n");
        return FLUX_ERR_BUILD;
    }

    {
        const char *test_src = "/tmp/flux_toolchain_test.c";
        FILE *tf = fopen(test_src, "w");
        if (!tf) { fprintf(stderr, "flux: can't write to /tmp\n"); return FLUX_ERR_GENERAL; }
        fprintf(tf, "#include <stdio.h>\nint main(void){return 0;}\n");
        fclose(tf);
        int tc = system("gcc /tmp/flux_toolchain_test.c -c -o /tmp/flux_toolchain_test.o >/dev/null 2>&1");
        remove(test_src);
        remove("/tmp/flux_toolchain_test.o");
        if (tc != 0) {
            fprintf(stderr, "flux: gcc can't find standard headers (e.g. stdio.h)\n");
            fprintf(stderr, "hint: the C library / headers on this system are incomplete; check kira-base\n");
            return FLUX_ERR_BUILD;
        }
    }

    printf("[flux] updating flux %s -> %s\n", FLUX_VERSION, version);

    const char *scratch = "/tmp/flux-selfupdate";
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", scratch);
    system(cmd);

    snprintf(cmd, sizeof(cmd),
        "git clone --depth 1 --branch \"%s\" \"%s\" \"%s\"",
        tag, FLUX_REPO_URL, scratch);
    if (system(cmd) != 0) {
        fprintf(stderr, "flux: failed to fetch flux source\n");
        return FLUX_ERR_NETWORK;
    }

    printf("[flux] building...\n");
    snprintf(cmd, sizeof(cmd), "cd \"%s\" && make", scratch);
    if (system(cmd) != 0) {
        fprintf(stderr, "flux: build failed\n");
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", scratch);
        system(cmd);
        return FLUX_ERR_BUILD;
    }

    char new_bin[FLUX_MAX_PATH_LEN];
    snprintf(new_bin, sizeof(new_bin), "%s/build/flux", scratch);
    struct stat st;
    if (stat(new_bin, &st) != 0) {
        fprintf(stderr, "flux: build did not produce a binary\n");
        return FLUX_ERR_BUILD;
    }

    char current[FLUX_MAX_PATH_LEN];
    current_binary_path(current, sizeof(current));

    char staged[FLUX_MAX_PATH_LEN + 8];
    char backup[FLUX_MAX_PATH_LEN + 8];
    snprintf(staged, sizeof(staged), "%s.new", current);
    snprintf(backup, sizeof(backup), "%s.bak", current);

    snprintf(cmd, sizeof(cmd), "cp \"%s\" \"%s\" && chmod 755 \"%s\"", new_bin, staged, staged);
    if (system(cmd) != 0) {
        fprintf(stderr, "flux: failed to stage new binary (need root?)\n");
        return FLUX_ERR_PERMISSION;
    }

    snprintf(cmd, sizeof(cmd), "cp \"%s\" \"%s\"", current, backup);
    system(cmd);

    if (rename(staged, current) != 0) {
        fprintf(stderr, "flux: failed to install new binary\n");
        remove(staged);
        return FLUX_ERR_GENERAL;
    }

    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", scratch);
    system(cmd);

    printf("[flux] updated to %s (previous binary kept at %s)\n", version, backup);
    return FLUX_ERR_NONE;
}
