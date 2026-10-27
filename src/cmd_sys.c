#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
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

typedef struct {
    char name[128];
    time_t mtime;
} kernel_version_t;

#define FLUX_KERNEL_CLEAN_MAX_VERSIONS 64
#define FLUX_KERNEL_CLEAN_DEFAULT_KEEP 2

// a kernel-update never removes the kernel it's replacing (kept on purpose, for rollback -
// see flux_kernel_update's own comment on old_modules_path), so without this every update
// just adds another ~90-190MB under /lib/modules + /boot forever. Never a candidate: the
// currently *running* kernel (uname -r), regardless of how old or how it sorts by mtime
static int sys_clean_kernel(int argc, char **argv) {
    static const char *clean_usage = "flux sys clean kernel [--keep N] [-y]";
    int keep = FLUX_KERNEL_CLEAN_DEFAULT_KEEP;
    int yes = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) {
            yes = 1;
        } else if (strcmp(argv[i], "--keep") == 0 && i + 1 < argc) {
            keep = atoi(argv[i + 1]);
            i++;
        } else {
            flux_usage_error(clean_usage);
            return FLUX_ERR_USAGE;
        }
    }
    if (keep < 1) {
        flux_err("--keep must be at least 1 - the running kernel is never removed regardless");
        return FLUX_ERR_USAGE;
    }

    FILE *f = popen("uname -r", "r");
    if (!f) { flux_err("failed to read current kernel version"); return FLUX_ERR_GENERAL; }
    char current[128] = {0};
    fgets(current, sizeof(current), f);
    pclose(f);
    strip_newline(current);
    if (strlen(current) == 0) { flux_err("failed to read current kernel version"); return FLUX_ERR_GENERAL; }

    DIR *d = opendir("/lib/modules");
    if (!d) { flux_err("/lib/modules not found"); return FLUX_ERR_GENERAL; }

    kernel_version_t versions[FLUX_KERNEL_CLEAN_MAX_VERSIONS];
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && count < FLUX_KERNEL_CLEAN_MAX_VERSIONS) {
        if (entry->d_name[0] == '.') continue;
        if (strcmp(entry->d_name, current) == 0) continue;

        char path[256];
        snprintf(path, sizeof(path), "/lib/modules/%s", entry->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        strncpy(versions[count].name, entry->d_name, sizeof(versions[count].name) - 1);
        versions[count].mtime = st.st_mtime;
        count++;
    }
    closedir(d);

    // newest (by mtime) first
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (versions[j].mtime > versions[i].mtime) {
                kernel_version_t tmp = versions[i];
                versions[i] = versions[j];
                versions[j] = tmp;
            }

    // keep already counts the running kernel itself, which was never in this list to begin with
    int keep_old = keep - 1;
    if (keep_old < 0) keep_old = 0;
    int remove_count = count - keep_old;
    if (remove_count <= 0) {
        flux_ok("nothing to clean - %d old kernel%s already within the keep-%d limit", count, count == 1 ? "" : "s", keep);
        return FLUX_ERR_NONE;
    }

    flux_action("The following old kernels will be removed (keeping %s, the running one, plus %d more recent):", current, keep_old);
    for (int i = keep_old; i < count; i++)
        printf("  - %s\n", versions[i].name);

    if (!yes) {
        printf("Proceed? [y/N] ");
        char answer[8];
        if (!fgets(answer, sizeof(answer), stdin) || (answer[0] != 'y' && answer[0] != 'Y')) {
            flux_warn("aborted");
            return FLUX_ERR_NONE;
        }
    }

    int removed = 0;
    for (int i = keep_old; i < count; i++) {
        char modpath[256], vmlinuz[256], initrd[256], cmd[896];
        snprintf(modpath, sizeof(modpath), "/lib/modules/%s", versions[i].name);
        snprintf(vmlinuz, sizeof(vmlinuz), "/boot/vmlinuz-%s", versions[i].name);
        snprintf(initrd, sizeof(initrd), "/boot/initrd.img-%s", versions[i].name);
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"; rm -f \"%s\" \"%s\"", modpath, vmlinuz, initrd);

        if (system(cmd) == 0) {
            flux_step("removed %s", versions[i].name);
            removed++;
        } else {
            flux_warn("failed to fully remove %s (need root?)", versions[i].name);
        }
    }

    if (removed > 0) {
        if (system("command -v grub-mkconfig >/dev/null 2>&1") == 0) {
            flux_step("updating grub...");
            system("grub-mkconfig -o /boot/grub/grub.cfg");
        } else if (system("command -v update-grub >/dev/null 2>&1") == 0) {
            flux_step("updating grub...");
            system("update-grub");
        }
    }

    flux_ok("removed %d old kernel%s", removed, removed == 1 ? "" : "s");
    return FLUX_ERR_NONE;
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

    if (strcmp(argv[0], "clean") == 0) {
        if (argc < 2 || strcmp(argv[1], "kernel") != 0) {
            flux_usage_error("flux sys clean kernel [--keep N] [-y]");
            return FLUX_ERR_USAGE;
        }
        return sys_clean_kernel(argc - 2, argv + 2);
    }

    flux_usage_error(usage);
    return FLUX_ERR_USAGE;
}
