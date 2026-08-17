#include <stdio.h>
#include "../include/flux.h"
#include "../include/util.h"

int flux_autoremove(int argc, char **argv, const char *usage) {
    (void)argc;
    (void)argv;
    (void)usage;

    flux_action("Scanning for orphaned dependencies");

    int removed = 0;
    if (flux_autoremove_orphans(&removed) != FLUX_ERR_NONE) {
        flux_err("autoremove failed");
        return FLUX_ERR_GENERAL;
    }

    if (removed > 0)
        flux_ok("removed %d orphaned dependency package(s)", removed);
    else
        flux_ok("nothing to remove");

    return FLUX_ERR_NONE;
}
