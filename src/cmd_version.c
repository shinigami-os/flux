#include <stdio.h>
#include "../include/flux.h"

int flux_version(int argc, char **argv, const char *usage) {
    (void)argc;
    (void)argv;
    (void)usage;

    printf("flux %s\n", FLUX_VERSION);
    return FLUX_ERR_NONE;
}
