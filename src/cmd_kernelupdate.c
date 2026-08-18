#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../include/flux.h"
#include "../include/util.h"

static int read_current_kernel(char *out, size_t outlen) {
    FILE *f = popen("uname -r", "r");
    if (!f) return FLUX_ERR_GENERAL;
    int ok = fgets(out, outlen, f) != NULL;
    pclose(f);
    if (!ok) return FLUX_ERR_GENERAL;
    strip_newline(out);
    return strlen(out) > 0 ? FLUX_ERR_NONE : FLUX_ERR_GENERAL;
}

int flux_fetch_latest_kernel_version(const char *cache_url, char *out, size_t outlen) {
    char url[FLUX_MAX_URL_LEN + 32];
    snprintf(url, sizeof(url), "%s/kira-kernel/latest", cache_url);

    const char *tmp = "/tmp/flux-kernel-latest";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "curl -fsSL --max-time 15 -o \"%s\" \"%s\" 2>/dev/null", tmp, url);
    if (system(cmd) != 0) return FLUX_ERR_NETWORK;

    FILE *f = fopen(tmp, "r");
    if (!f) { remove(tmp); return FLUX_ERR_GENERAL; }
    int ok = fgets(out, outlen, f) != NULL;
    fclose(f);
    remove(tmp);
    if (!ok) return FLUX_ERR_GENERAL;
    strip_newline(out);
    return strlen(out) > 0 ? FLUX_ERR_NONE : FLUX_ERR_GENERAL;
}

static void parse_kernel_version(const char *release,
                                  char *linux_ver, size_t lv_len,
                                  char *shinigami_ver, size_t sv_len) {
    // format: 6.12.85-shinigami-26.07
    const char *sep = strstr(release, "-shinigami-");
    if (sep) {
        size_t lv_sz = (size_t)(sep - release);
        if (lv_sz >= lv_len) lv_sz = lv_len - 1;
        strncpy(linux_ver, release, lv_sz);
        linux_ver[lv_sz] = '\0';
        strncpy(shinigami_ver, sep + 11, sv_len - 1);
    } else {
        strncpy(linux_ver, release, lv_len - 1);
        shinigami_ver[0] = '\0';
    }
}

static int download_signed_kernel(const char *cache_url, const char *version,
                                   const char *dest_dir, const char *pub_path) {
    char filename[128];
    snprintf(filename, sizeof(filename), "kira-kernel-%s.tar.gz", version);

    char dest[FLUX_MAX_PATH_LEN];
    snprintf(dest, sizeof(dest), "%s/%s", dest_dir, filename);

    char url[FLUX_MAX_URL_LEN + FLUX_MAX_PATH_LEN];
    char cmd[1024];

    snprintf(url, sizeof(url), "%s/kira-kernel/%s/%s", cache_url, version, filename);
    if (flux_download(url, dest) != FLUX_ERR_NONE) return FLUX_ERR_NETWORK;

    char sig_dest[FLUX_MAX_PATH_LEN + 8];
    snprintf(sig_dest, sizeof(sig_dest), "%s.minisig", dest);
    snprintf(url, sizeof(url), "%s/kira-kernel/%s/%s.minisig", cache_url, version, filename);
    snprintf(cmd, sizeof(cmd), "curl -fL --max-time 60 -o \"%s\" \"%s\"", sig_dest, url);
    if (system(cmd) != 0) return FLUX_ERR_NETWORK;

    return flux_cache_verify(dest, pub_path);
}

int flux_kernel_update(int argc, char **argv, const char *usage) {
    int force = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--force") == 0)
            force = 1;
        else {
            flux_usage_error(usage);
            return FLUX_ERR_USAGE;
        }
    }

    char current[128];
    if (read_current_kernel(current, sizeof(current)) != FLUX_ERR_NONE) {
        flux_err("failed to read current kernel version");
        return FLUX_ERR_GENERAL;
    }

    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;
    if (strlen(config.binary_cache_url) == 0) {
        flux_err("binary_cache_url not set in flux.conf");
        return FLUX_ERR_GENERAL;
    }

    flux_action("Checking for a newer kernel");
    char latest[128];
    if (flux_fetch_latest_kernel_version(config.binary_cache_url, latest, sizeof(latest)) != FLUX_ERR_NONE) {
        flux_err("could not fetch latest kernel version from cache");
        return FLUX_ERR_NETWORK;
    }

    if (strcmp(current, latest) == 0 && !force) {
        flux_ok("kernel already up to date (%s)", current);
        return FLUX_ERR_NONE;
    }

    char cur_linux[64], cur_shina[32], new_linux[64], new_shina[32];
    parse_kernel_version(current, cur_linux, sizeof(cur_linux), cur_shina, sizeof(cur_shina));
    parse_kernel_version(latest,  new_linux, sizeof(new_linux), new_shina, sizeof(new_shina));

    flux_action("Updating kernel");
    if (strcmp(cur_linux, new_linux) != 0)
        printf("  linux:     %s -> %s\n", cur_linux, new_linux);
    if (strcmp(cur_shina, new_shina) != 0)
        printf("  shinigami: %s -> %s\n", cur_shina, new_shina);

    const char *scratch = "/tmp/flux-kernel-update";
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && mkdir -p \"%s\"", scratch, scratch);
    system(cmd);

    flux_step("downloading kira-kernel-%s.tar.gz...", latest);
    if (download_signed_kernel(config.binary_cache_url, latest, scratch, config.flux_pub_path) != FLUX_ERR_NONE) {
        flux_err("failed to fetch or verify kernel archive");
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", scratch);
        system(cmd);
        return FLUX_ERR_NETWORK;
    }

    flux_step("installing kernel %s...", latest);
    char archive[FLUX_MAX_PATH_LEN];
    snprintf(archive, sizeof(archive), "%s/kira-kernel-%s.tar.gz", scratch, latest);
    snprintf(cmd, sizeof(cmd), "tar -xzf \"%s\" -C /", archive);
    if (system(cmd) != 0) {
        flux_err("failed to extract kernel archive");
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", scratch);
        system(cmd);
        return FLUX_ERR_GENERAL;
    }

    flux_step("running depmod...");
    snprintf(cmd, sizeof(cmd), "depmod -a \"%s\"", latest);
    if (system(cmd) != 0)
        flux_warn("depmod failed, modprobe may not find all modules");

    struct stat old_mod_st;
    char old_modules_path[256];
    snprintf(old_modules_path, sizeof(old_modules_path), "/lib/modules/%s", current);
    if (strcmp(current, latest) != 0 && stat(old_modules_path, &old_mod_st) == 0)
        flux_step("old modules kept at %s (for rollback)", old_modules_path);

    if (system("command -v grub-mkconfig >/dev/null 2>&1") == 0) {
        flux_step("updating grub...");
        system("grub-mkconfig -o /boot/grub/grub.cfg");
    } else if (system("command -v update-grub >/dev/null 2>&1") == 0) {
        flux_step("updating grub...");
        system("update-grub");
    } else {
        flux_warn("grub not found, update your bootloader manually");
    }

    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", scratch);
    system(cmd);

    flux_ok("kernel updated to %s", latest);
    flux_warn("reboot required to boot the new kernel");
    return FLUX_ERR_NONE;
}
