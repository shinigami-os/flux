#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/flux.h"
#include "../include/util.h"

int flux_diff(int argc, char **argv, const char *usage) {
    (void)argv;
    if (argc != 0) {
        flux_usage_error(usage);
        return FLUX_ERR_USAGE;
    }

    flux_action("Checking installed files against what was shipped");

    // heap: FLUX_MAX_INSTALL_QUEUE modified_file_t entries is sizeable, and there's no
    // need to keep it around once printed
    flux_modified_file_t *modified = malloc((size_t)FLUX_MAX_INSTALL_QUEUE * sizeof(flux_modified_file_t));
    if (!modified) {
        flux_err("out of memory");
        return FLUX_ERR_GENERAL;
    }

    int count = 0;
    if (flux_diff_modified_files(modified, FLUX_MAX_INSTALL_QUEUE, &count) != FLUX_ERR_NONE) {
        flux_err("diff failed");
        free(modified);
        return FLUX_ERR_GENERAL;
    }

    if (count == 0) {
        flux_ok("nothing has been locally modified since it was installed");
    } else {
        flux_table_row_t *rows = malloc((size_t)count * sizeof(flux_table_row_t));
        if (rows) {
            for (int i = 0; i < count; i++) {
                strncpy(rows[i].col1, modified[i].pkg, FLUX_MAX_NAME_LEN - 1);
                strncpy(rows[i].col2, modified[i].path, sizeof(rows[i].col2) - 1);
            }
            char title[64];
            snprintf(title, sizeof(title), "%d locally modified file%s", count, count == 1 ? "" : "s");
            printf("\n");
            flux_print_table(title, rows, count);
            free(rows);
        }
    }

    // matters either way: a clean report could just mean nothing has a baseline to compare against yet
    flux_warn("a package with no recorded checksums yet is invisible here - run 'flux checksum-backfill' if this looks incomplete");

    free(modified);
    return FLUX_ERR_NONE;
}
