#include <stdio.h>
#include "../include/flux.h"
#include "../include/util.h"

int flux_autoremove(int argc, char **argv, const char *usage) {
    (void)argc;
    (void)argv;
    (void)usage;

    printf("[flux] scanning for orphaned dependencies...\n");

    int removed = 0;
    if (flux_autoremove_orphans(&removed) != FLUX_ERR_NONE) {
        fprintf(stderr, "flux: autoremove failed\n");
        return FLUX_ERR_GENERAL;
    }

    if (removed > 0)
        printf("[flux] removed %d orphaned dependency package(s)\n", removed);
    else
        printf("[flux] nothing to remove\n");

    return FLUX_ERR_NONE;
}
