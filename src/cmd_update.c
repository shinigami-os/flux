#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../include/flux.h"
#include "../include/util.h"

static void check_for_flux_release(void);
static void check_for_base_release(void);

int flux_update(int argc, char **argv, const char *usage) {
    (void)argc;
    (void)argv;
    (void)usage;

    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    char git_dir[FLUX_MAX_PATH_LEN + 8];
    snprintf(git_dir, sizeof(git_dir), "%s/.git", config.local_repo_path);
    struct stat st;
    if (stat(git_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        printf("[flux] downloading recipe repo...\n");

        char tmp_tar[FLUX_MAX_PATH_LEN];
        snprintf(tmp_tar, sizeof(tmp_tar), "/tmp/flux-recipes.tar.gz");

        char cmd[FLUX_MAX_PATH_LEN * 2 + FLUX_MAX_URL_LEN + 64];
        snprintf(cmd, sizeof(cmd),
                "curl -L -o \"%s\" \"https://github.com/shinigami-os/flux-recipes/archive/refs/heads/main.tar.gz\"",
                tmp_tar);
        if (system(cmd) != 0) {
            fprintf(stderr, "flux: failed to download recipe repo\n");
            return FLUX_ERR_NETWORK;
        }

        // create destination and extract
        snprintf(cmd, sizeof(cmd),
                "mkdir -p \"%s\" && tar -xf \"%s\" -C \"%s\" --strip-components=1",
                config.local_repo_path, tmp_tar, config.local_repo_path);
        if (system(cmd) != 0) {
            fprintf(stderr, "flux: failed to extract recipe repo\n");
            remove(tmp_tar);
            return FLUX_ERR_GENERAL;
        }

        remove(tmp_tar);
    }
    else{
        printf("[flux] syncing recipe repo...\n");

        char cmd[FLUX_MAX_PATH_LEN + 32];
        snprintf(cmd, sizeof(cmd), "git -C \"%s\" pull", config.local_repo_path);

        int ret = system(cmd);
        if (ret != 0) {
            fprintf(stderr, "flux: failed to sync recipe repo\n");
            return FLUX_ERR_NETWORK;
        }
    }
    printf("[flux] recipe repo up to date\n");

    check_for_flux_release();
    check_for_base_release();
    return FLUX_ERR_NONE;
}

static void check_for_flux_release(void) {
    char tag[64] = {0};
    if (flux_fetch_latest_git_tag(FLUX_REPO_URL, tag, sizeof(tag)) != FLUX_ERR_NONE) return;

    const char *version = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
    if (strcmp(version, FLUX_VERSION) != 0) {
        printf("[flux] a newer flux release is available: %s (current: %s)\n", version, FLUX_VERSION);
        printf("[flux] run 'flux self-update' to update\n");
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

    const char *version = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
    if (strcmp(version, current) != 0) {
        printf("[flux] a newer kira-base release is available: %s (current: %s)\n", version, current);
        printf("[flux] run 'flux base-update' to update\n");
    }
}