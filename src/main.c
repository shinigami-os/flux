#include <stdio.h>
#include <string.h>
#include "../include/flux.h"

#define ANSI_BOLD    "\033[1m"
#define ANSI_RESET   "\033[0m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_DIM     "\033[2m"

int flux_install(int argc, char **argv, const char *usage);
int flux_search(int argc, char **argv, const char *usage);
int flux_build(int argc, char **argv, const char *usage);
int flux_remove(int argc, char **argv, const char *usage);
int flux_update(int argc, char **argv, const char *usage);
int flux_info(int argc, char **argv, const char *usage);
int flux_cache(int argc, char **argv, const char *usage);
int flux_compat(int argc, char **argv, const char *usage);

flux_cmd_t commands[] = {
    {"install", flux_install, "Install a package (from cache or compile)", "flux install <pkg>"},
    {"search", flux_search, "Search available recipes", "flux search <query>"},
    {"build", flux_build, "Force local compilation regardless of cache", "flux build <pkg>"},
    {"remove", flux_remove, "Remove a package and orphaned dependencies", "flux remove <pkg>"},
    {"update", flux_update, "Sync recipe repo and update all installed packages", "flux update"},
    {"info", flux_info, "Show package details, dependencies, recipes", "flux info <pkg>"},
    {"cache", flux_cache, "Manage binary cache", "flux cache <subcommand>"},
    {"compat", flux_compat, "Install via Debian compat container", "flux compat <pkg>"},
    {NULL, NULL, NULL, NULL}
};

void flux_usage() {
    printf("\n");
    printf(ANSI_BOLD ANSI_MAGENTA "flux" ANSI_RESET " - the " ANSI_BOLD "Kira Linux" ANSI_RESET " package manager\n");
    printf(ANSI_DIM "by OxoGhost\n" ANSI_RESET);
    printf("\n");

    printf(ANSI_BOLD ANSI_YELLOW "USAGE\n" ANSI_RESET);
    printf("  flux " ANSI_CYAN "<command>" ANSI_RESET " [arguments]\n");
    printf("\n");

    printf(ANSI_BOLD ANSI_YELLOW "COMMANDS\n" ANSI_RESET);
    for (int i = 0; commands[i].handler != NULL; i++) {
        printf("  " ANSI_CYAN "%-30s" ANSI_RESET ANSI_GREEN "%s\n" ANSI_RESET, commands[i].usage, commands[i].desc);
    }
    printf("\n");

    printf(ANSI_DIM "Run 'flux help <command>' for more information on a command.\n" ANSI_RESET);
    printf("\n");
}

int main(int argc, char **argv) {
    if(argc < 2){
        flux_usage();
        return FLUX_ERR_USAGE;
    }
    char *command = argv[1];
    for(int i = 0; commands[i].handler != NULL; i++){
        if(strcmp(command, commands[i].name) == 0){
            return commands[i].handler(argc - 2, argv + 2, commands[i].usage);
        }
    }
    printf("flux: unknown command: %s\n", command);
    return FLUX_ERR_USAGE;
}
