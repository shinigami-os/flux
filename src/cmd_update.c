#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../include/flux.h"
#include "../include/util.h"
#include "../include/parser.h"
#include "../include/alpine.h"

int flux_install(int argc, char **argv, const char *usage);

static void check_for_flux_release(void);
static void check_for_base_release(void);
static void check_for_kernel_release(const flux_config_t *config);
static void check_musl_soname(const flux_config_t *config);
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

    flux_action("Updating Kira Linux");

    if (stat(git_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        // self-heals a leftover tarball checkout from before git existed
        if (system("command -v git >/dev/null 2>&1") == 0) {
            flux_step("cloning recipe repo...");

            char cmd[FLUX_MAX_PATH_LEN * 2 + FLUX_MAX_URL_LEN + 64];
            snprintf(cmd, sizeof(cmd),
                    "rm -rf \"%s\" && git clone --depth 1 --branch \"%s\" \"%s\" \"%s\"",
                    config.local_repo_path, config.recipes_branch, FLUX_RECIPES_REPO_URL, config.local_repo_path);
            if (system(cmd) != 0) {
                flux_err("failed to clone recipe repo");
                return FLUX_ERR_NETWORK;
            }
        } else {
            flux_step("downloading recipe repo...");

            char tmp_tar[FLUX_MAX_PATH_LEN];
            snprintf(tmp_tar, sizeof(tmp_tar), "/tmp/flux-recipes.tar.gz");

            char archive_url[FLUX_MAX_URL_LEN + 96];
            snprintf(archive_url, sizeof(archive_url), "%s/archive/refs/heads/%s.tar.gz", FLUX_RECIPES_REPO_URL, config.recipes_branch);
            if (flux_download(archive_url, tmp_tar) != FLUX_ERR_NONE) {
                flux_err("failed to download recipe repo");
                return FLUX_ERR_NETWORK;
            }

            char cmd[FLUX_MAX_PATH_LEN * 2 + FLUX_MAX_URL_LEN + 64];
            snprintf(cmd, sizeof(cmd),
                    "mkdir -p \"%s\" && tar -xf \"%s\" -C \"%s\" --strip-components=1",
                    config.local_repo_path, tmp_tar, config.local_repo_path);
            if (system(cmd) != 0) {
                flux_err("failed to extract recipe repo");
                remove(tmp_tar);
                return FLUX_ERR_GENERAL;
            }

            remove(tmp_tar);
        }
    }
    else{
        have_old_head = (get_git_head(config.local_repo_path, old_head, sizeof(old_head)) == FLUX_ERR_NONE);

        flux_step("syncing recipe repo (%s)...", config.recipes_branch);

        // fetch+reset onto the configured branch, not a plain "pull" - that would just advance whatever's already checked out, missing a branch change in flux.conf
        char cmd[FLUX_MAX_PATH_LEN * 2 + 256];
        snprintf(cmd, sizeof(cmd), "git -C \"%s\" fetch --depth 1 origin \"%s\" && git -C \"%s\" checkout -B \"%s\" FETCH_HEAD",
                 config.local_repo_path, config.recipes_branch, config.local_repo_path, config.recipes_branch);

        int ret = system(cmd);
        if (ret != 0) {
            flux_err("failed to sync recipe repo");
            return FLUX_ERR_NETWORK;
        }
    }
    flux_ok("recipe repo up to date");

    if (have_old_head) {
        char new_head[64] = {0};
        if (get_git_head(config.local_repo_path, new_head, sizeof(new_head)) == FLUX_ERR_NONE &&
            strcmp(old_head, new_head) != 0) {

            char outdated[FLUX_MAX_INSTALL_QUEUE][FLUX_MAX_NAME_LEN];
            int outdated_count = 0;
            report_and_collect_updates(&config, old_head, new_head, outdated, FLUX_MAX_INSTALL_QUEUE, &outdated_count);

            if (outdated_count > 0) {
                if (install_updates) {
                    flux_action("Installing %d update%s", outdated_count, outdated_count == 1 ? "" : "s");
                    for (int i = 0; i < outdated_count; i++) {
                        char *install_argv[] = { "-y", "-f", outdated[i] };
                        int err = flux_install(3, install_argv, "flux install [-y] [-f] <pkg>");
                        if (err != FLUX_ERR_NONE)
                            flux_err("failed to update '%s'", outdated[i]);
                    }
                } else {
                    printf("\nRun 'flux update -i' to install %s.\n",
                           outdated_count == 1 ? "it" : "them");
                }
            } else {
                flux_ok("no installed packages have available updates");
            }
        }
    }

    update_flatpak();
    check_for_flux_release();
    check_for_base_release();
    check_for_kernel_release(&config);
    check_musl_soname(&config);
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
    flux_table_row_t rows[FLUX_MAX_INSTALL_QUEUE];
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

        strncpy(rows[*count].col1, pkg, FLUX_MAX_NAME_LEN - 1);
        snprintf(rows[*count].col2, sizeof(rows[*count].col2), "%s -> %s", info.version, recipe.version);
        strncpy(outdated[*count], pkg, FLUX_MAX_NAME_LEN - 1);
        (*count)++;
    }
    pclose(f);

    if (*count > 0) {
        char title[64];
        snprintf(title, sizeof(title), "%d update%s available", *count, *count == 1 ? "" : "s");
        printf("\n");
        flux_print_table(title, rows, *count);
    }
    return FLUX_ERR_NONE;
}

// musl's SONAME never bumps, so so:-resolution (alpine_resolve.c) needs no version pinning as long as this exists
static void check_musl_soname(const flux_config_t *config) {
    char arch[ALPINE_MAX_ARCH_LEN];
    if (alpine_arch_from_target(config->package_target, arch, sizeof(arch)) != FLUX_ERR_NONE) return;

    char path[64];
    snprintf(path, sizeof(path), "/lib/ld-musl-%s.so.1", arch);

    struct stat st;
    if (stat(path, &st) != 0)
        flux_warn("%s not found - Alpine packages will fail to resolve so:libc.musl-%s.so.1", path, arch);
}

static void update_flatpak(void) {
    if (system("command -v flatpak >/dev/null 2>&1") != 0) return;
    flux_step("updating flatpak apps...");
    system("flatpak update -y");
}

static void check_for_flux_release(void) {
    char tag[64] = {0};
    if (flux_fetch_latest_git_tag(FLUX_REPO_URL, tag, sizeof(tag)) != FLUX_ERR_NONE) return;

    const char *version = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
    if (strcmp(version, FLUX_VERSION) != 0) {
        flux_warn("a newer flux release is available: %s (current: %s)", version, FLUX_VERSION);
        printf("  run 'flux self-update' to update\n");
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
        flux_warn("a newer kira-base release is available: %s (current: %s)", version, current);
        printf("  run 'flux base-update' to update\n");
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
        flux_warn("a newer kernel is available: %s (current: %s)", latest, current);
        printf("  run 'flux kernel-update' to update\n");
    }
}