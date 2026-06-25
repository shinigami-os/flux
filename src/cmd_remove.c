#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../include/flux.h"
#include "../include/util.h"

int flux_remove(int argc, char **argv, const char *usage) {
    int autoremove = 0;
    while (argc >= 1 && (strcmp(argv[0], "-a") == 0 || strcmp(argv[0], "--autoremove") == 0)) {
        autoremove = 1;
        argv++;
        argc--;
    }

    if (argc < 1) {
        flux_usage_error(usage);
        return FLUX_ERR_USAGE;
    }

    const char *pkg = argv[0];
    printf("[flux] removing: %s\n", pkg);

    if (!flux_db_is_installed(pkg)) {
        fprintf(stderr, "flux: %s is not installed\n", pkg);
        return FLUX_ERR_NOT_FOUND;
    }

    if (flux_db_remove(pkg) != FLUX_ERR_NONE) {
        fprintf(stderr, "flux: failed to remove %s\n", pkg);
        return FLUX_ERR_GENERAL;
    }

    printf("[flux] %s removed successfully\n", pkg);

    if (autoremove) {
        int removed = 0;
        flux_autoremove_orphans(&removed);
        if (removed > 0)
            printf("[flux] removed %d orphaned dependency package(s)\n", removed);
        else
            printf("[flux] no orphaned dependencies to remove\n");
    }

    return FLUX_ERR_NONE;
}