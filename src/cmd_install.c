#include <stdio.h>
#include "../include/flux.h"
#include "../include/util.h"

int flux_install(int argc, char **argv, const char *usage) {
    (void)usage;
    if (argc >= 1){
        printf("[flux] would install: %s\n", argv[0]);
        return FLUX_ERR_NONE;
    }
    flux_usage_error(usage);
    return FLUX_ERR_USAGE;
}