#include <stdio.h>
#include <string.h>
#include "../include/flux.h"
#include "../include/util.h"


int flux_install(int argc, char **argv, const char *usage);
int flux_search(int argc, char **argv, const char *usage);
int flux_build(int argc, char **argv, const char *usage);
int flux_remove(int argc, char **argv, const char *usage);
int flux_check(int argc, char **argv, const char *usage);
int flux_upgrade(int argc, char **argv, const char *usage);
int flux_info(int argc, char **argv, const char *usage);
int flux_list(int argc, char **argv, const char *usage);
int flux_cache(int argc, char **argv, const char *usage);
int flux_autoremove(int argc, char **argv, const char *usage);
int flux_version(int argc, char **argv, const char *usage);
int flux_sys(int argc, char **argv, const char *usage);
int flux_why(int argc, char **argv, const char *usage);
int flux_checksum_backfill(int argc, char **argv, const char *usage);

flux_cmd_t commands[] = {
    {"install", flux_install, "Install one or more packages (from cache or compile)", "flux install [-y] [-f] [-v] <pkgs>"},
    {"search", flux_search, "Search available recipes", "flux search <query>"},
    {"build", flux_build, "Cross-compile a kotodama recipe into the bootstrap sysroot", "flux build --cross <pkg>"},
    {"remove", flux_remove, "Remove a package, optionally its orphaned deps", "flux remove [-a] <pkg>"},
    {"autoremove", flux_autoremove, "Remove all orphaned auto-installed dependencies", "flux autoremove [-n|--dry-run]"},
    {"why", flux_why, "Trace the dependency chain(s) keeping a package installed", "flux why <pkg>"},
    {"check", flux_check, "Check for package updates (read-only; add --files to also check for local modifications)", "flux check [--files]"},
    {"upgrade", flux_upgrade, "Install whatever 'flux check' found", "flux upgrade"},
    {"checksum-backfill", flux_checksum_backfill, "Record checksums for installed packages that predate conffile protection", "flux checksum-backfill"},
    {"info", flux_info, "Show package details, dependencies, recipes", "flux info <pkg>"},
    {"list", flux_list, "List installed packages", "flux list [-a]"},
    {"cache", flux_cache, "Manage binary cache", "flux cache <subcommand>"},
    {"version", flux_version, "Show the installed flux version", "flux version"},
    {"sys", flux_sys, "Check or upgrade flux itself, kira-base, and the kernel", "flux sys check | flux sys upgrade [flux|base|kernel]"},
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
