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

static int read_current_version(char *out, size_t outlen) {
    FILE *f = fopen("/etc/kira-release", "r");
    if (!f) return FLUX_ERR_NOT_FOUND;

    char line[128];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        strip_newline(line);
        if (strncmp(line, "KIRA_BASE_VERSION=", 18) == 0) {
            strncpy(out, line + 18, outlen - 1);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found ? FLUX_ERR_NONE : FLUX_ERR_NOT_FOUND;
}

static int download_signed(const char *base_url, const char *version, const char *filename, const char *dest_dir, const char *pub_path) {
    char dest[FLUX_MAX_PATH_LEN];
    snprintf(dest, sizeof(dest), "%s/%s", dest_dir, filename);

    char url[FLUX_MAX_URL_LEN + FLUX_MAX_PATH_LEN];
    char cmd[1024];

    snprintf(url, sizeof(url), "%s/kira-base/%s/%s", base_url, version, filename);
    if (flux_download(url, dest) != FLUX_ERR_NONE) return FLUX_ERR_NETWORK;

    snprintf(url, sizeof(url), "%s/kira-base/%s/%s.minisig", base_url, version, filename);
    char sig_dest[FLUX_MAX_PATH_LEN + 8];
    snprintf(sig_dest, sizeof(sig_dest), "%s.minisig", dest);
    snprintf(cmd, sizeof(cmd), "curl -fL --max-time 60 -o \"%s\" \"%s\"", sig_dest, url);
    if (system(cmd) != 0) return FLUX_ERR_NETWORK;

    return flux_cache_verify(dest, pub_path);
}

static int atomic_replace(const char *src, const char *dst) {
    char staged[FLUX_MAX_PATH_LEN + 8];
    snprintf(staged, sizeof(staged), "%s.new", dst);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cp -a \"%s\" \"%s\"", src, staged);
    if (system(cmd) != 0) return FLUX_ERR_GENERAL;

    if (rename(staged, dst) != 0) {
        remove(staged);
        return FLUX_ERR_GENERAL;
    }
    return FLUX_ERR_NONE;
}

#define FLUX_MAX_BASE_SERVICES 64
#define FLUX_BASE_SERVICES_RECORD "/var/lib/flux/kira-base-services"

// reconciles /etc/sv against kira-base's shipped services only, leaving flux-package-owned service dirs (dbus, networkmanager, etc.) untouched
static void sync_base_services(const char *rootfs_dir, char (*new_services)[FLUX_MAX_NAME_LEN], int new_count) {
    FILE *rf = fopen(FLUX_BASE_SERVICES_RECORD, "r");
    if (rf) {
        char line[FLUX_MAX_NAME_LEN];
        while (fgets(line, sizeof(line), rf)) {
            strip_newline(line);
            if (strlen(line) == 0) continue;

            int still_shipped = 0;
            for (int i = 0; i < new_count; i++) {
                if (strcmp(line, new_services[i]) == 0) { still_shipped = 1; break; }
            }
            if (!still_shipped) {
                flux_step("removing service no longer shipped by kira-base: %s", line);
                char cmd[FLUX_MAX_PATH_LEN + 64];
                snprintf(cmd, sizeof(cmd), "sv down /etc/sv/%s >/dev/null 2>&1; rm -rf /etc/sv/%s", line, line);
                system(cmd);
            }
        }
        fclose(rf);
    }

    for (int i = 0; i < new_count; i++) {
        char src[FLUX_MAX_PATH_LEN + 32];
        char dst[FLUX_MAX_PATH_LEN];
        snprintf(src, sizeof(src), "%s/etc/sv/%s", rootfs_dir, new_services[i]);
        snprintf(dst, sizeof(dst), "/etc/sv/%s", new_services[i]);

        struct stat st;
        if (stat(src, &st) != 0) continue;

        char cmd[(FLUX_MAX_PATH_LEN + 32) * 3];
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && cp -a \"%s\" \"%s\"", dst, src, dst);
        if (system(cmd) == 0) {
            flux_step("updated service /etc/sv/%s", new_services[i]);
            // greetd's %post-install disables getty-tty1 (they fight over tty1); redo that here since this restore just undid it
            if (strcmp(new_services[i], "getty-tty1") == 0) {
                struct stat greetd_st;
                if (stat("/etc/sv/greetd/run", &greetd_st) == 0)
                    system("touch /etc/sv/getty-tty1/down; sv down /etc/sv/getty-tty1 >/dev/null 2>&1");
            }
        }
    }

    mkdir("/var/lib/flux", 0755);
    FILE *wf = fopen(FLUX_BASE_SERVICES_RECORD, "w");
    if (wf) {
        for (int i = 0; i < new_count; i++)
            fprintf(wf, "%s\n", new_services[i]);
        fclose(wf);
    }
}

static void apply_manifest(const char *rootfs_dir, int *boot_pending) {
    char manifest_path[FLUX_MAX_PATH_LEN + 32];
    snprintf(manifest_path, sizeof(manifest_path), "%s/etc/kira-update-manifest", rootfs_dir);

    FILE *mf = fopen(manifest_path, "r");
    if (!mf) {
        flux_err("kira-update-manifest missing from release, nothing applied");
        return;
    }

    char base_services[FLUX_MAX_BASE_SERVICES][FLUX_MAX_NAME_LEN];
    int base_service_count = 0;

    char line[300];
    while (fgets(line, sizeof(line), mf)) {
        strip_newline(line);
        if (strlen(line) == 0) continue;

        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = '\0';
        const char *category = line;
        const char *target = sp + 1;

        char src[FLUX_MAX_PATH_LEN + 16];
        snprintf(src, sizeof(src), "%s%s", rootfs_dir, target);

        struct stat st;
        if (stat(src, &st) != 0) {
            flux_warn("%s missing from release, skipping", target);
            continue;
        }

        if (strcmp(category, "live") == 0) {
            if (atomic_replace(src, target) == FLUX_ERR_NONE)
                flux_step("updated %s", target);
        } else if (strncmp(category, "restart:", 8) == 0) {
            if (atomic_replace(src, target) == FLUX_ERR_NONE) {
                flux_step("updated %s", target);
                char svc_cmd[256];
                snprintf(svc_cmd, sizeof(svc_cmd), "sv restart /etc/sv/%s", category + 8);
                system(svc_cmd);
            }
        } else if (strcmp(category, "boot") == 0) {
            if (atomic_replace(src, target) == FLUX_ERR_NONE) {
                flux_step("staged %s (applies on next boot)", target);
                *boot_pending = 1;
            }
        } else if (strcmp(category, "service") == 0) {
            if (base_service_count < FLUX_MAX_BASE_SERVICES) {
                const char *name = strrchr(target, '/');
                name = name ? name + 1 : target;
                strncpy(base_services[base_service_count], name, FLUX_MAX_NAME_LEN - 1);
                base_services[base_service_count][FLUX_MAX_NAME_LEN - 1] = '\0';
                base_service_count++;
            }
        }
    }
    fclose(mf);

    sync_base_services(rootfs_dir, base_services, base_service_count);
}

static void apply_initramfs(const char *scratch, int *boot_pending) {
    char kernel_release[128] = {0};
    FILE *kf = popen("uname -r", "r");
    if (!kf) return;
    if (fgets(kernel_release, sizeof(kernel_release), kf)) strip_newline(kernel_release);
    pclose(kf);
    if (strlen(kernel_release) == 0) return;

    char initrd_path[FLUX_MAX_PATH_LEN];
    snprintf(initrd_path, sizeof(initrd_path), "/boot/initrd.img-%s", kernel_release);

    char new_initramfs[FLUX_MAX_PATH_LEN];
    snprintf(new_initramfs, sizeof(new_initramfs), "%s/initramfs.cpio.gz", scratch);

    if (atomic_replace(new_initramfs, initrd_path) == FLUX_ERR_NONE) {
        flux_step("staged %s (applies on next boot)", initrd_path);
        *boot_pending = 1;
    } else {
        flux_warn("failed to stage new initramfs at %s", initrd_path);
    }
}

int flux_base_update(int argc, char **argv, const char *usage) {
    int force = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--force") == 0)
            force = 1;
        else {
            flux_usage_error(usage);
            return FLUX_ERR_USAGE;
        }
    }

    char current[64];
    if (read_current_version(current, sizeof(current)) != FLUX_ERR_NONE) {
        flux_err("/etc/kira-release not found, is this a Kira system?");
        return FLUX_ERR_NOT_FOUND;
    }

    flux_action("Checking for a newer kira-base release");
    char tag[64];
    int fetch_err = flux_fetch_latest_git_tag(KIRA_BASE_REPO_URL, tag, sizeof(tag));
    if (fetch_err == FLUX_ERR_NOT_FOUND) {
        flux_warn("no release tags found at %s", KIRA_BASE_REPO_URL);
        return FLUX_ERR_NOT_FOUND;
    }
    if (fetch_err != FLUX_ERR_NONE) {
        flux_err("could not reach %s to check for updates", KIRA_BASE_REPO_URL);
        return FLUX_ERR_NETWORK;
    }

    const char *version = strip_v(tag);
    if (strcmp(version, current) == 0 && !force) {
        flux_ok("kira-base already up to date (%s)", current);
        return FLUX_ERR_NONE;
    }

    flux_action("Updating kira-base %s -> %s", current, version);

    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;
    if (strlen(config.binary_cache_url) == 0) {
        flux_err("binary_cache_url not set in flux.conf");
        return FLUX_ERR_GENERAL;
    }

    const char *scratch = "/tmp/flux-base-update";
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && mkdir -p \"%s\"", scratch, scratch);
    system(cmd);

    flux_step("downloading rootfs.tar.gz...");
    if (download_signed(config.binary_cache_url, version, "rootfs.tar.gz", scratch, config.flux_pub_path) != FLUX_ERR_NONE) {
        flux_err("failed to fetch or verify rootfs.tar.gz");
        return FLUX_ERR_NETWORK;
    }

    flux_step("downloading initramfs.cpio.gz...");
    if (download_signed(config.binary_cache_url, version, "initramfs.cpio.gz", scratch, config.flux_pub_path) != FLUX_ERR_NONE) {
        flux_err("failed to fetch or verify initramfs.cpio.gz");
        return FLUX_ERR_NETWORK;
    }

    char rootfs_dir[FLUX_MAX_PATH_LEN];
    snprintf(rootfs_dir, sizeof(rootfs_dir), "%s/rootfs", scratch);
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\" && tar -xzf \"%s/rootfs.tar.gz\" -C \"%s\"", rootfs_dir, scratch, rootfs_dir);
    if (system(cmd) != 0) {
        flux_err("failed to extract rootfs.tar.gz");
        return FLUX_ERR_GENERAL;
    }

    int boot_pending = 0;
    apply_manifest(rootfs_dir, &boot_pending);
    apply_initramfs(scratch, &boot_pending);

    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", scratch);
    system(cmd);

    FILE *rf = fopen("/etc/kira-release", "w");
    if (rf) {
        fprintf(rf, "KIRA_BASE_VERSION=%s\n", version);
        fclose(rf);
    }

    flux_ok("kira-base updated to %s", version);
    if (boot_pending)
        flux_warn("reboot required to apply the new init/runit binaries and initramfs");

    return FLUX_ERR_NONE;
}
