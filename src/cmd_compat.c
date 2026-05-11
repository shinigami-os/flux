#include <stdio.h>
#include "../include/flux.h"

int flux_compat(int argc, char **argv, const char *usage){
    (void)argc;
    (void)argv;
    (void)usage;
    printf("[flux] stub: compat not yet implemented");
    return FLUX_ERR_NONE;
}