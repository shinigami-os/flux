#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/flux.h"
#include "../include/util.h"

// read-only: never installs or changes anything. 'flux upgrade' acts on exactly what this reports.
int flux_check(int argc, char **argv, const char *usage) {
    int want_files = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--files") == 0)
            want_files = 1;
        else {
            flux_usage_error(usage);
            return FLUX_ERR_USAGE;
        }
    }

    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    flux_action("Checking for package updates");

    char outdated[FLUX_MAX_INSTALL_QUEUE][FLUX_MAX_NAME_LEN];
    int outdated_count = 0;
    flux_collect_package_updates(&config, outdated, FLUX_MAX_INSTALL_QUEUE, &outdated_count);

    if (outdated_count > 0)
        printf("\nRun 'flux upgrade' to install %s.\n", outdated_count == 1 ? "it" : "them");
    else
        flux_ok("no installed packages have available updates");

    if (want_files) {
        printf("\n");
        flux_action("Checking installed files against what was shipped");

        // heap: FLUX_MAX_INSTALL_QUEUE modified_file_t entries is sizeable, and there's no
        // need to keep it around once printed
        flux_modified_file_t *modified = malloc((size_t)FLUX_MAX_INSTALL_QUEUE * sizeof(flux_modified_file_t));
        if (!modified) {
            flux_err("out of memory");
            return FLUX_ERR_GENERAL;
        }

        int mcount = 0;
        if (flux_diff_modified_files(modified, FLUX_MAX_INSTALL_QUEUE, &mcount) != FLUX_ERR_NONE) {
            flux_err("file diff failed");
            free(modified);
            return FLUX_ERR_GENERAL;
        }

        if (mcount == 0) {
            flux_ok("nothing has been locally modified since it was installed");
        } else {
            flux_table_row_t *rows = malloc((size_t)mcount * sizeof(flux_table_row_t));
            if (rows) {
                for (int i = 0; i < mcount; i++) {
                    strncpy(rows[i].col1, modified[i].pkg, FLUX_MAX_NAME_LEN - 1);
                    strncpy(rows[i].col2, modified[i].path, sizeof(rows[i].col2) - 1);
                }
                char title[64];
                snprintf(title, sizeof(title), "%d locally modified file%s", mcount, mcount == 1 ? "" : "s");
                printf("\n");
                flux_print_table(title, rows, mcount);
                free(rows);
            }
        }

        // matters either way: a clean report could just mean nothing has a baseline to compare against yet
        flux_warn("a package with no recorded checksums yet is invisible here - run 'flux checksum-backfill' if this looks incomplete");
        free(modified);
    }

    return FLUX_ERR_NONE;
}
