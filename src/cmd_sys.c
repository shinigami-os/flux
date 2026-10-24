#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../include/flux.h"
#include "../include/util.h"
#include "../include/alpine.h"

int flux_self_update(int argc, char **argv, const char *usage);
int flux_base_update(int argc, char **argv, const char *usage);
int flux_kernel_update(int argc, char **argv, const char *usage);

static const char *strip_v(const char *tag) {
    return (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
}

static void check_for_flux_release(void) {
    char tag[64] = {0};
    if (flux_fetch_latest_git_tag(FLUX_REPO_URL, tag, sizeof(tag)) != FLUX_ERR_NONE) return;

    const char *version = strip_v(tag);
    if (strcmp(version, FLUX_VERSION) != 0) {
        flux_warn("a newer flux release is available: %s (current: %s)", version, FLUX_VERSION);
        printf("  run 'flux sys upgrade flux' to update\n");
    }
}

static void check_for_base_release(void) {
    FILE *rf = fopen("/etc/kira-release", "r");
    if (!rf) return;

    char line[128];
    char current[64] = {0};
    while (fgets(line, sizeof(line), rf)) {
        strip_newline(line);
        if (strncmp(line, "KIRA_BASE_VERSION=", 18) == 0) {
            strncpy(current, line + 18, sizeof(current) - 1);
            break;
        }
    }
    fclose(rf);
    if (strlen(current) == 0) return;

    char tag[64] = {0};
    if (flux_fetch_latest_git_tag(KIRA_BASE_REPO_URL, tag, sizeof(tag)) != FLUX_ERR_NONE) return;

    const char *version = strip_v(tag);
    if (strcmp(version, current) != 0) {
        flux_warn("a newer kira-base release is available: %s (current: %s)", version, current);
        printf("  run 'flux sys upgrade base' to update\n");
    }
}

static void check_for_kernel_release(const flux_config_t *config) {
    if (strlen(config->binary_cache_url) == 0) return;

    FILE *f = popen("uname -r", "r");
    if (!f) return;
    char current[128] = {0};
    if (fgets(current, sizeof(current), f))
        strip_newline(current);
    pclose(f);
    if (strlen(current) == 0) return;

    char latest[128] = {0};
    if (flux_fetch_latest_kernel_version(config->binary_cache_url, latest, sizeof(latest)) != FLUX_ERR_NONE) return;

    if (strcmp(current, latest) != 0) {
        flux_warn("a newer kernel is available: %s (current: %s)", latest, current);
        printf("  run 'flux sys upgrade kernel' to update\n");
    }
}

// musl's SONAME never bumps, so so:-resolution (alpine_resolve.c) needs no version pinning as long as this exists
static void check_musl_soname(const flux_config_t *config) {
    char arch[ALPINE_MAX_ARCH_LEN];
    if (alpine_arch_from_target(config->package_target, arch, sizeof(arch)) != FLUX_ERR_NONE) return;

    char path[64];
    snprintf(path, sizeof(path), "/lib/ld-musl-%s.so.1", arch);

    struct stat st;
    if (stat(path, &st) != 0)
        flux_warn("%s not found - Alpine packages will fail to resolve so:libc.musl-%s.so.1", path, arch);
}

static int sys_check(void) {
    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    flux_action("Checking flux, kira-base, and the kernel");
    check_for_flux_release();
    check_for_base_release();
    check_for_kernel_release(&config);
    check_musl_soname(&config);
    return FLUX_ERR_NONE;
}

static int sys_upgrade(int argc, char **argv) {
    static const char *upgrade_usage = "flux sys upgrade [flux|base|kernel] [-f]";

    if (argc == 0) {
        // same batch-resilience philosophy as a package batch install: one layer
        // failing to fetch/build shouldn't stop the other two from being tried
        int flux_err_r = flux_self_update(0, NULL, upgrade_usage);
        int base_err = flux_base_update(0, NULL, upgrade_usage);
        int kernel_err = flux_kernel_update(0, NULL, upgrade_usage);
        if (flux_err_r != FLUX_ERR_NONE || base_err != FLUX_ERR_NONE || kernel_err != FLUX_ERR_NONE)
            return FLUX_ERR_GENERAL;
        return FLUX_ERR_NONE;
    }

    const char *target = argv[0];
    int rest_argc = argc - 1;
    char **rest_argv = argv + 1;

    if (strcmp(target, "flux") == 0) return flux_self_update(rest_argc, rest_argv, upgrade_usage);
    if (strcmp(target, "base") == 0) return flux_base_update(rest_argc, rest_argv, upgrade_usage);
    if (strcmp(target, "kernel") == 0) return flux_kernel_update(rest_argc, rest_argv, upgrade_usage);

    flux_usage_error(upgrade_usage);
    return FLUX_ERR_USAGE;
}

int flux_sys(int argc, char **argv, const char *usage) {
    if (argc < 1) {
        flux_usage_error(usage);
        return FLUX_ERR_USAGE;
    }

    if (strcmp(argv[0], "check") == 0) {
        if (argc != 1) {
            flux_usage_error(usage);
            return FLUX_ERR_USAGE;
        }
        return sys_check();
    }

    if (strcmp(argv[0], "upgrade") == 0)
        return sys_upgrade(argc - 1, argv + 1);

    flux_usage_error(usage);
    return FLUX_ERR_USAGE;
}
