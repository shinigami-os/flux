#include <stdio.h>
#include "../include/flux.h"
#include "../include/util.h"

extern int flux_install(int argc, char **argv, const char *usage);

int flux_build(int argc, char **argv, const char *usage) {
    if (argc < 1) {
        flux_usage_error(usage);
        return FLUX_ERR_USAGE;
    }
    printf("[flux] force building: %s\n", argv[0]);
    printf("[flux] note: flux build will diverge from flux install once binary cache is implemented\n");
    // reuse install logic for now
    return flux_install(argc, argv, usage);
}