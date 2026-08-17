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
    int force = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--force") == 0)
            force = 1;
        else {
            flux_usage_error(usage);
            return FLUX_ERR_USAGE;
        }
    }

    flux_action("Checking for a newer flux release");
    char tag[64];
    int fetch_err = flux_fetch_latest_git_tag(FLUX_REPO_URL, tag, sizeof(tag));
    if (fetch_err == FLUX_ERR_NOT_FOUND) {
        flux_warn("no release tags found at %s", FLUX_REPO_URL);
        return FLUX_ERR_NOT_FOUND;
    }
    if (fetch_err != FLUX_ERR_NONE) {
        flux_err("could not reach %s to check for updates", FLUX_REPO_URL);
        return FLUX_ERR_NETWORK;
    }

    const char *version = strip_v(tag);
    if (strcmp(version, FLUX_VERSION) == 0 && !force) {
        flux_ok("already up to date (%s)", FLUX_VERSION);
        return FLUX_ERR_NONE;
    }

    if (system("command -v gcc >/dev/null 2>&1") != 0 ||
        system("command -v make >/dev/null 2>&1") != 0) {
        flux_err("gcc/make not found, can't build flux from source");
        flux_err("hint: run 'flux install build-essential' first");
        return FLUX_ERR_BUILD;
    }

    int need_explicit_includes = 0;
    {
        const char *test_src = "/tmp/flux_toolchain_test.c";
        FILE *tf = fopen(test_src, "w");
        if (!tf) { flux_err("can't write to /tmp"); return FLUX_ERR_GENERAL; }
        fprintf(tf, "#include <stdio.h>\nint main(void){return 0;}\n");
        fclose(tf);

        int tc = system("gcc /tmp/flux_toolchain_test.c -c -o /tmp/flux_toolchain_test.o >/dev/null 2>&1");
        if (tc != 0) {
            /* gcc may have wrong default sysroot from cross-build; try explicit include path */
            tc = system("gcc -I/usr/include /tmp/flux_toolchain_test.c -c -o /tmp/flux_toolchain_test.o >/dev/null 2>&1");
            if (tc == 0) {
                need_explicit_includes = 1;
                flux_warn("gcc needs -I/usr/include (musl sysroot mismatch, will add to build)");
            }
        }

        remove(test_src);
        remove("/tmp/flux_toolchain_test.o");

        if (tc != 0) {
            struct stat hst;
            if (stat("/usr/include/stdio.h", &hst) != 0) {
                flux_err("/usr/include/stdio.h not found");
                flux_err("hint: musl dev headers are missing, run 'flux base-update' or reinstall kira-base");
            } else {
                flux_err("gcc cannot compile against /usr/include headers");
                flux_err("hint: try 'flux install -f gcc' to reinstall the compiler");
            }
            return FLUX_ERR_BUILD;
        }
    }

    flux_action("Updating flux %s -> %s", FLUX_VERSION, version);

    const char *scratch = "/tmp/flux-selfupdate";
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", scratch);
    system(cmd);

    snprintf(cmd, sizeof(cmd),
        "git clone --depth 1 --branch \"%s\" \"%s\" \"%s\"",
        tag, FLUX_REPO_URL, scratch);
    if (system(cmd) != 0) {
        flux_err("failed to fetch flux source");
        return FLUX_ERR_NETWORK;
    }

    flux_step("building...");
    if (need_explicit_includes)
        snprintf(cmd, sizeof(cmd), "cd \"%s\" && make CFLAGS=\"-Wall -Wextra -pedantic -std=c11 -I/usr/include\"", scratch);
    else
        snprintf(cmd, sizeof(cmd), "cd \"%s\" && make", scratch);
    if (system(cmd) != 0) {
        flux_err("build failed");
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", scratch);
        system(cmd);
        return FLUX_ERR_BUILD;
    }

    char new_bin[FLUX_MAX_PATH_LEN];
    snprintf(new_bin, sizeof(new_bin), "%s/build/flux", scratch);
    struct stat st;
    if (stat(new_bin, &st) != 0) {
        flux_err("build did not produce a binary");
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
        flux_err("failed to stage new binary (need root?)");
        return FLUX_ERR_PERMISSION;
    }

    snprintf(cmd, sizeof(cmd), "cp \"%s\" \"%s\"", current, backup);
    system(cmd);

    if (rename(staged, current) != 0) {
        flux_err("failed to install new binary");
        remove(staged);
        return FLUX_ERR_GENERAL;
    }

    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", scratch);
    system(cmd);

    flux_ok("updated to %s (previous binary kept at %s)", version, backup);
    return FLUX_ERR_NONE;
}
