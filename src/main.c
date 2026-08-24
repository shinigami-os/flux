#include <stdio.h>
#include <string.h>
#include "../include/flux.h"
#include "../include/util.h"


int flux_install(int argc, char **argv, const char *usage);
int flux_search(int argc, char **argv, const char *usage);
int flux_build(int argc, char **argv, const char *usage);
int flux_remove(int argc, char **argv, const char *usage);
int flux_update(int argc, char **argv, const char *usage);
int flux_info(int argc, char **argv, const char *usage);
int flux_list(int argc, char **argv, const char *usage);
int flux_cache(int argc, char **argv, const char *usage);
int flux_compat(int argc, char **argv, const char *usage);
int flux_autoremove(int argc, char **argv, const char *usage);
int flux_version(int argc, char **argv, const char *usage);
int flux_self_update(int argc, char **argv, const char *usage);
int flux_base_update(int argc, char **argv, const char *usage);
int flux_kernel_update(int argc, char **argv, const char *usage);

flux_cmd_t commands[] = {
    {"install", flux_install, "Install one or more packages (from cache or compile)", "flux install [-y] [-f] <pkgs>"},
    {"search", flux_search, "Search available recipes", "flux search <query>"},
    {"build", flux_build, "Force local compilation regardless of cache", "flux build [--cross] <pkg>"},
    {"remove", flux_remove, "Remove a package, optionally its orphaned deps", "flux remove [-a] <pkg>"},
    {"autoremove", flux_autoremove, "Remove all orphaned auto-installed dependencies", "flux autoremove"},
    {"update", flux_update, "Sync recipe repo, report package updates, and check for newer flux/kira-base releases", "flux update [-i]"},
    {"info", flux_info, "Show package details, dependencies, recipes", "flux info <pkg>"},
    {"list", flux_list, "List installed packages", "flux list [-a]"},
    {"cache", flux_cache, "Manage binary cache", "flux cache <subcommand>"},
    {"compat", flux_compat, "Install via Debian compat container", "flux compat <pkg>"},
    {"version", flux_version, "Show the installed flux version", "flux version"},
    {"self-update", flux_self_update, "Rebuild and replace flux from the latest release", "flux self-update [-f]"},
    {"base-update", flux_base_update, "Update kira-base's core image to the latest release", "flux base-update [-f]"},
    {"kernel-update", flux_kernel_update, "Update the Shinigami kernel to the latest release", "flux kernel-update [-f]"},
    {NULL, NULL, NULL, NULL}
};

void flux_usage() {
    int c = flux_colors_enabled();
    const char *bold = c ? "\033[1m" : "", *reset = c ? "\033[0m" : "";
    const char *cyan = c ? "\033[36m" : "", *yellow = c ? "\033[33m" : "";
    const char *green = c ? "\033[32m" : "", *magenta = c ? "\033[35m" : "", *dim = c ? "\033[2m" : "";

    printf("\n");
    printf("%s%sflux%s - the %sKira Linux%s package manager\n", bold, magenta, reset, bold, reset);
    printf("%sby OxoGhost%s\n", dim, reset);
    printf("\n");

    printf("%s%sUSAGE%s\n", bold, yellow, reset);
    printf("  flux %s<command>%s [arguments]\n", cyan, reset);
    printf("\n");

    printf("%s%sCOMMANDS%s\n", bold, yellow, reset);
    for (int i = 0; commands[i].handler != NULL; i++) {
        printf("  %s%-30s%s%s%s%s\n", cyan, commands[i].usage, reset, green, commands[i].desc, reset);
    }
    printf("\n");

    printf("%sRun 'flux help <command>' for more information on a command.%s\n", dim, reset);
    printf("\n");
}

int main(int argc, char **argv) {
    if(argc < 2){
        flux_usage();
        return FLUX_ERR_USAGE;
    }
    char *command = argv[1];
    if (strcmp(command, "--version") == 0 || strcmp(command, "-v") == 0)
        command = "version";
    for(int i = 0; commands[i].handler != NULL; i++){
        if(strcmp(command, commands[i].name) == 0){
            return commands[i].handler(argc - 2, argv + 2, commands[i].usage);
        }
    }
    flux_err("unknown command: %s", command);
    return FLUX_ERR_USAGE;
}
