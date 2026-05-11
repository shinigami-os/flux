#include <stdio.h>
#include "../include/flux.h"
#include "../include/util.h"

int flux_remove(int argc, char **argv, const char *usage) {
    if (argc >= 1){
        printf("[flux] would remove: %s\n", argv[0]);
        return FLUX_ERR_NONE;
    }
    flux_usage_error(usage);
    return FLUX_ERR_USAGE;
}