#include <stdio.h>
#include <string.h>
#include "../include/flux.h"
#include "../include/util.h"

int flux_install(int argc, char **argv, const char *usage);

// acts on exactly what 'flux check' would report - same two-step split as 'apk update'/'apk upgrade'
int flux_upgrade(int argc, char **argv, const char *usage) {
    (void)argv;
    if (argc != 0) {
        flux_usage_error(usage);
        return FLUX_ERR_USAGE;
    }

    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    flux_action("Checking for package updates");

    char outdated[FLUX_MAX_INSTALL_QUEUE][FLUX_MAX_NAME_LEN];
    int outdated_count = 0;
    flux_collect_package_updates(&config, outdated, FLUX_MAX_INSTALL_QUEUE, &outdated_count);

    if (outdated_count == 0) {
        flux_ok("no installed packages have available updates");
        return FLUX_ERR_NONE;
    }

    flux_action("Installing %d update%s", outdated_count, outdated_count == 1 ? "" : "s");
    // one batched call, same as "flux install pkg1 pkg2 ..." by hand - a plain install
    // already reinstalls whenever the version moved on, -f is only for forcing a rebuild
    // of a package already at the latest version
    char *install_argv[FLUX_MAX_INSTALL_QUEUE + 1];
    install_argv[0] = "-y";
    for (int i = 0; i < outdated_count; i++)
        install_argv[i + 1] = outdated[i];
    int err = flux_install(outdated_count + 1, install_argv, "flux install [-y] [-f] <pkg>");
    if (err != FLUX_ERR_NONE) {
        flux_err("failed to install one or more updates");
        return err;
    }
    return FLUX_ERR_NONE;
}
