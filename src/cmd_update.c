#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../include/flux.h"
#include "../include/util.h"

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
        printf("[flux] creating recipe repo at %s\n", config.local_repo_path);
        char cmd[FLUX_MAX_PATH_LEN + FLUX_MAX_URL_LEN + 32];
        snprintf(cmd, sizeof(cmd), "git clone \"%s\" \"%s\"", config.remote_repo_url, config.local_repo_path);
        int ret = system(cmd);
        if (ret != 0) {
            fprintf(stderr, "flux: failed to clone recipe repo\n");
            return FLUX_ERR_NETWORK;
        }
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
    return FLUX_ERR_NONE;
}