#include <stdio.h>
#include "../include/flux.h"
#include "../include/util.h"

int flux_compat(int argc, char **argv, const char *usage){
    (void)argc;
    (void)argv;
    (void)usage;
    flux_warn("compat: not yet implemented");
    return FLUX_ERR_NONE;
}