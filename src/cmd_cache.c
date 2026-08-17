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
            flux_warn("cache clean --all: not yet implemented");
            return FLUX_ERR_NONE;
        }
        if (argc >= 2 && strcmp(argv[1], "--unused") == 0) {
            flux_warn("cache clean --unused: not yet implemented");
            return FLUX_ERR_NONE;
        }
        flux_warn("cache clean: not yet implemented");
        return FLUX_ERR_NONE;
    }

    flux_err("unknown cache subcommand: %s", argv[0]);
    return FLUX_ERR_USAGE;
}