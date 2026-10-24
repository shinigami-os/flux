#include "../include/flux.h"
#include "../include/util.h"

int flux_checksum_backfill(int argc, char **argv, const char *usage) {
    (void)argv;
    if (argc != 0) {
        flux_usage_error(usage);
        return FLUX_ERR_USAGE;
    }

    flux_action("Backfilling checksums for installed packages");

    int backfilled = 0;
    if (flux_backfill_checksums(&backfilled) != FLUX_ERR_NONE) {
        flux_err("checksum backfill failed");
        return FLUX_ERR_GENERAL;
    }

    if (backfilled > 0)
        flux_ok("backfilled checksums for %d package(s)", backfilled);
    else
        flux_ok("every installed package already has checksums");

    return FLUX_ERR_NONE;
}
