#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../include/flux.h"
#include "../include/util.h"
#include "../include/parser.h"

int flux_install(int argc, char **argv, const char *usage);

static void check_for_flux_release(void);
static void check_for_base_release(void);
static void check_for_kernel_release(const flux_config_t *config);
static void update_flatpak(void);
static int  get_git_head(const char *repo_path, char *out, size_t outlen);
static int  report_and_collect_updates(const flux_config_t *config, const char *old_head, const char *new_head,
                                        char outdated[][FLUX_MAX_NAME_LEN], int max, int *count);

int flux_update(int argc, char **argv, const char *usage) {
    int install_updates = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--install") == 0) {
            install_updates = 1;
        } else {
            flux_usage_error(usage);
            return FLUX_ERR_USAGE;
        }
    }

    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    char git_dir[FLUX_MAX_PATH_LEN + 8];
    snprintf(git_dir, sizeof(git_dir), "%s/.git", config.local_repo_path);
    struct stat st;
    char old_head[64] = {0};
    int have_old_head = 0;

    if (stat(git_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        // self-heals a leftover tarball checkout from before git existed
        if (system("command -v git >/dev/null 2>&1") == 0) {
            printf("[flux] cloning recipe repo...\n");

            char cmd[FLUX_MAX_PATH_LEN * 2 + FLUX_MAX_URL_LEN + 64];
            snprintf(cmd, sizeof(cmd),
                    "rm -rf \"%s\" && git clone --depth 1 \"%s\" \"%s\"",
                    config.local_repo_path, FLUX_RECIPES_REPO_URL, config.local_repo_path);
            if (system(cmd) != 0) {
                fprintf(stderr, "flux: failed to clone recipe repo\n");
                return FLUX_ERR_NETWORK;
            }
        } else {
            printf("[flux] downloading recipe repo...\n");

            char tmp_tar[FLUX_MAX_PATH_LEN];
            snprintf(tmp_tar, sizeof(tmp_tar), "/tmp/flux-recipes.tar.gz");

            char cmd[FLUX_MAX_PATH_LEN * 2 + FLUX_MAX_URL_LEN + 64];
            snprintf(cmd, sizeof(cmd),
                    "curl -L -o \"%s\" \"%s/archive/refs/heads/main.tar.gz\"",
                    tmp_tar, FLUX_RECIPES_REPO_URL);
            if (system(cmd) != 0) {
                fprintf(stderr, "flux: failed to download recipe repo\n");
                return FLUX_ERR_NETWORK;
            }

            snprintf(cmd, sizeof(cmd),
                    "mkdir -p \"%s\" && tar -xf \"%s\" -C \"%s\" --strip-components=1",
                    config.local_repo_path, tmp_tar, config.local_repo_path);
            if (system(cmd) != 0) {
                fprintf(stderr, "flux: failed to extract recipe repo\n");
                remove(tmp_tar);
                return FLUX_ERR_GENERAL;
            }

            remove(tmp_tar);
        }
    }
    else{
        have_old_head = (get_git_head(config.local_repo_path, old_head, sizeof(old_head)) == FLUX_ERR_NONE);

        printf("[flux] syncing recipe repo...\n");

        char cmd[FLUX_MAX_PATH_LEN + 32];
        snprintf(cmd, sizeof(cmd), "git -C \"%s\" pull", config.local_repo_path);

        int ret = system(cmd);
        if (ret != 0) {
            fprintf(stderr, "flux: failed to sync recipe repo\n");
            return FLUX_ERR_NETWORK;
        }
    }
    printf("[flux] recipe repo up to date\n");

    if (have_old_head) {
        char new_head[64] = {0};
        if (get_git_head(config.local_repo_path, new_head, sizeof(new_head)) == FLUX_ERR_NONE &&
            strcmp(old_head, new_head) != 0) {

            char outdated[FLUX_MAX_INSTALL_QUEUE][FLUX_MAX_NAME_LEN];
            int outdated_count = 0;
            report_and_collect_updates(&config, old_head, new_head, outdated, FLUX_MAX_INSTALL_QUEUE, &outdated_count);

            if (outdated_count > 0) {
                if (install_updates) {
                    printf("\n[flux] installing %d update%s...\n", outdated_count, outdated_count == 1 ? "" : "s");
                    for (int i = 0; i < outdated_count; i++) {
                        char *install_argv[] = { "-y", "-f", outdated[i] };
                        int err = flux_install(3, install_argv, "flux install [-y] [-f] <pkg>");
                        if (err != FLUX_ERR_NONE)
                            fprintf(stderr, "flux: failed to update '%s'\n", outdated[i]);
                    }
                } else {
                    printf("\n[flux] run 'flux update -i' to install %s\n",
                           outdated_count == 1 ? "it" : "them");
                }
            } else {
                printf("[flux] no installed packages have available updates\n");
            }
        }
    }

    update_flatpak();
    check_for_flux_release();
    check_for_base_release();
    check_for_kernel_release(&config);
    return FLUX_ERR_NONE;
}

static int get_git_head(const char *repo_path, char *out, size_t outlen) {
    char cmd[FLUX_MAX_PATH_LEN + 32];
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" rev-parse HEAD", repo_path);
    FILE *f = popen(cmd, "r");
    if (!f) return FLUX_ERR_GENERAL;
    int ok = (fgets(out, outlen, f) != NULL);
    pclose(f);
    if (!ok) return FLUX_ERR_GENERAL;
    strip_newline(out);
    return strlen(out) > 0 ? FLUX_ERR_NONE : FLUX_ERR_GENERAL;
}

// only reports changed <pkg>/kotodama files where pkg is installed and its version actually changed - "update available" should only mean something for packages on this system
static int report_and_collect_updates(const flux_config_t *config, const char *old_head, const char *new_head,
                                       char outdated[][FLUX_MAX_NAME_LEN], int max, int *count) {
    *count = 0;

    char cmd[FLUX_MAX_PATH_LEN + 128];
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" diff --name-only %s %s -- '*/kotodama'",
             config->local_repo_path, old_head, new_head);
    FILE *f = popen(cmd, "r");
    if (!f) return FLUX_ERR_GENERAL;

    char line[FLUX_MAX_PATH_LEN];
    int printed_header = 0;
    while (*count < max && fgets(line, sizeof(line), f)) {
        strip_newline(line);
        char *slash = strchr(line, '/');
        if (!slash || strcmp(slash + 1, "kotodama") != 0) continue;
        *slash = '\0';
        const char *pkg = line;

        if (!flux_db_is_installed(pkg)) continue;

        flux_pkg_info_t info;
        if (flux_db_read_info(pkg, &info) != FLUX_ERR_NONE) continue;

        char koto_path[FLUX_MAX_PATH_LEN * 2 + 16];
        snprintf(koto_path, sizeof(koto_path), "%s/%s/kotodama", config->local_repo_path, pkg);
        flux_recipe_t recipe;
        memset(&recipe, 0, sizeof(recipe));
        if (parse_kotodama(&recipe, koto_path) != FLUX_ERR_NONE) continue;

        if (strcmp(info.version, recipe.version) == 0) continue;

        if (!printed_header) {
            printf("\n[flux] updates available:\n");
            printed_header = 1;
        }
        printf("  %-32s %s -> %s\n", pkg, info.version, recipe.version);

        strncpy(outdated[*count], pkg, FLUX_MAX_NAME_LEN - 1);
        (*count)++;
    }
    pclose(f);
    return FLUX_ERR_NONE;
}

static void update_flatpak(void) {
    if (system("command -v flatpak >/dev/null 2>&1") != 0) return;
    printf("[flux] updating flatpak apps...\n");
    system("flatpak update -y");
}

static void check_for_flux_release(void) {
    char tag[64] = {0};
    if (flux_fetch_latest_git_tag(FLUX_REPO_URL, tag, sizeof(tag)) != FLUX_ERR_NONE) return;

    const char *version = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
    if (strcmp(version, FLUX_VERSION) != 0) {
        printf("[flux] a newer flux release is available: %s (current: %s)\n", version, FLUX_VERSION);
        printf("[flux] run 'flux self-update' to update\n");
    }
}

static void check_for_base_release(void) {
    FILE *rf = fopen("/etc/kira-release", "r");
    if (!rf) return;

    char line[128];
    char current[64] = {0};
    while (fgets(line, sizeof(line), rf)) {
        strip_newline(line);
        if (strncmp(line, "KIRA_BASE_VERSION=", 18) == 0) {
            strncpy(current, line + 18, sizeof(current) - 1);
            break;
        }
    }
    fclose(rf);
    if (strlen(current) == 0) return;

    char tag[64] = {0};
    if (flux_fetch_latest_git_tag(KIRA_BASE_REPO_URL, tag, sizeof(tag)) != FLUX_ERR_NONE) return;

    const char *version = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
    if (strcmp(version, current) != 0) {
        printf("[flux] a newer kira-base release is available: %s (current: %s)\n", version, current);
        printf("[flux] run 'flux base-update' to update\n");
    }
}

static void check_for_kernel_release(const flux_config_t *config) {
    if (strlen(config->binary_cache_url) == 0) return;

    FILE *f = popen("uname -r", "r");
    if (!f) return;
    char current[128] = {0};
    if (fgets(current, sizeof(current), f))
        strip_newline(current);
    pclose(f);
    if (strlen(current) == 0) return;

    char latest[128] = {0};
    if (flux_fetch_latest_kernel_version(config->binary_cache_url, latest, sizeof(latest)) != FLUX_ERR_NONE) return;

    if (strcmp(current, latest) != 0) {
        printf("[flux] a newer kernel is available: %s (current: %s)\n", latest, current);
        printf("[flux] run 'flux kernel-update' to update\n");
    }
}