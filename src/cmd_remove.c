#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../include/flux.h"
#include "../include/util.h"

int flux_remove(int argc, char **argv, const char *usage) {
    int autoremove = 0;
    while (argc >= 1 && (strcmp(argv[0], "-a") == 0 || strcmp(argv[0], "--autoremove") == 0)) {
        autoremove = 1;
        argv++;
        argc--;
    }

    if (argc < 1) {
        flux_usage_error(usage);
        return FLUX_ERR_USAGE;
    }

    const char *pkg = argv[0];
    flux_action("Removing %s", pkg);

    if (!flux_db_is_installed(pkg)) {
        flux_err("%s is not installed", pkg);
        return FLUX_ERR_NOT_FOUND;
    }

    if (flux_db_remove(pkg) != FLUX_ERR_NONE) {
        flux_err("failed to remove %s", pkg);
        return FLUX_ERR_GENERAL;
    }

    flux_ok("%s removed successfully", pkg);

    if (autoremove) {
        int removed = 0;
        flux_autoremove_orphans(&removed);
        if (removed > 0)
            flux_ok("removed %d orphaned dependency package(s)", removed);
        else
            flux_step("no orphaned dependencies to remove");
    }

    return FLUX_ERR_NONE;
}