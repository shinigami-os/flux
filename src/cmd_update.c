#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/flux.h"
#include "../include/util.h"

int flux_update(int argc, char **argv, const char *usage) {
    (void)argc;
    (void)argv;
    (void)usage;

    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    printf("[flux] syncing recipe repo...\n");

    char cmd[FLUX_MAX_PATH_LEN + 32];
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" pull", config.local_repo_path);

    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "flux: failed to sync recipe repo\n");
        return FLUX_ERR_NETWORK;
    }

    printf("[flux] recipe repo up to date\n");
    return FLUX_ERR_NONE;
}