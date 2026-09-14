#include <stdio.h>
#include <string.h>
#include "../include/flux.h"
#include "../include/util.h"

int flux_autoremove(int argc, char **argv, const char *usage) {
    int dry_run = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else {
            flux_usage_error(usage);
            return FLUX_ERR_USAGE;
        }
    }

    flux_action(dry_run ? "Scanning for orphaned dependencies (dry run)" : "Scanning for orphaned dependencies");

    int removed = 0;
    if (flux_autoremove_orphans(&removed, dry_run) != FLUX_ERR_NONE) {
        flux_err("autoremove failed");
        return FLUX_ERR_GENERAL;
    }

    if (removed > 0)
        flux_ok(dry_run ? "%d orphaned dependency package(s) would be removed" : "removed %d orphaned dependency package(s)", removed);
    else
        flux_ok("nothing to remove");

    return FLUX_ERR_NONE;
}
