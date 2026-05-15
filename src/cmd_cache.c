#include <stdio.h>
#include <string.h>
#include "../include/flux.h"
#include "../include/util.h"

int flux_cache(int argc, char **argv, const char *usage) {
    if (argc < 1) {
        flux_usage_error(usage);
        return FLUX_ERR_USAGE;
    }

    if (strcmp(argv[0], "clean") == 0) {
        if (argc >= 2 && strcmp(argv[1], "--all") == 0) {
            printf("[flux] cache clean --all: stub\n");
            return FLUX_ERR_NONE;
        }
        if (argc >= 2 && strcmp(argv[1], "--unused") == 0) {
            printf("[flux] cache clean --unused: stub\n");
            return FLUX_ERR_NONE;
        }
        printf("[flux] cache clean: stub\n");
        return FLUX_ERR_NONE;
    }

    fprintf(stderr, "flux: unknown cache subcommand: %s\n", argv[0]);
    return FLUX_ERR_USAGE;
}