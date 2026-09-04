#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/util.h"
#include "../include/parser.h"
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <stdarg.h>

void flux_usage_error(const char *usage){
    printf("usage: %s\n", usage);
}

void strip_newline(char *s) {
    size_t len = strlen(s);
    if (len > 0 && s[len - 1] == '\n')
        s[len - 1] = '\0';
}

char *trim_left(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

void trim_right(char *s) {
    int len = (int)strlen(s) - 1;
    while (len >= 0 && (s[len] == ' ' || s[len] == '\t'))
        s[len--] = '\0';
}

int flux_load_config(flux_config_t *config) {
    FILE *f = fopen("/etc/flux/flux.conf", "r");
    if (!f) {
        flux_err("cannot open /etc/flux/flux.conf");
        return FLUX_ERR_GENERAL;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        strip_newline(line);

        char *trimmed = trim_left(line);
        if (*trimmed == '\0' || *trimmed == '#') continue;

        char *eq = strchr(trimmed, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trimmed;
        char *val = eq + 1;
        trim_right(key);
        val = trim_left(val);
        trim_right(val);

        if (strlen(key) == 0 || strlen(val) == 0) continue;

        if (strcmp(key, "local_repo_path") == 0) strncpy(config->local_repo_path, val, FLUX_MAX_PATH_LEN - 1);
        if (strcmp(key, "remote_repo_url") == 0) strncpy(config->remote_repo_url, val, FLUX_MAX_URL_LEN - 1);
        if (strcmp(key, "binary_cache_url") == 0) strncpy(config->binary_cache_url, val, FLUX_MAX_URL_LEN - 1); 
        if (strcmp(key, "default_build_flags")== 0) strncpy(config->default_build_flags,val, FLUX_MAX_CFLAGS_LEN - 1);
        if (strcmp(key, "flux_pub_path") == 0) strncpy(config->flux_pub_path, val, FLUX_MAX_PATH_LEN - 1);
        if (strcmp(key, "flux_secret_key_path") == 0) strncpy(config->flux_secret_key_path, val, FLUX_MAX_PATH_LEN - 1);
        if (strcmp(key, "cross_compile_prefix") == 0) strncpy(config->flux_cross_compile_prefix, val, FLUX_MAX_PATH_LEN - 1);
        if (strcmp(key, "cross_compile_sysroot") == 0) strncpy(config->flux_cross_compile_sysroot, val, FLUX_MAX_PATH_LEN - 1);
        if (strcmp(key, "cross_toolchain_path") == 0) strncpy(config->flux_cross_toolchain_path, val, FLUX_MAX_PATH_LEN - 1);
        if (strcmp(key, "cross_gcc_libpath") == 0) strncpy(config->flux_cross_gcc_libpath , val, FLUX_MAX_PATH_LEN - 1);
        if (strcmp(key, "package_target") == 0) strncpy(config->package_target, val, sizeof(config->package_target) - 1);
        if (strcmp(key, "alpine_mirror_url") == 0) strncpy(config->alpine_mirror_url, val, FLUX_MAX_URL_LEN - 1);
        if (strcmp(key, "alpine_branch") == 0) strncpy(config->alpine_branch, val, sizeof(config->alpine_branch) - 1);
        if (strcmp(key, "recipes_branch") == 0) strncpy(config->recipes_branch, val, sizeof(config->recipes_branch) - 1);
    }

    fclose(f);

    if (config->alpine_mirror_url[0] == '\0')
        strncpy(config->alpine_mirror_url, "https://dl-cdn.alpinelinux.org/alpine", FLUX_MAX_URL_LEN - 1);
    if (config->alpine_branch[0] == '\0')
        strncpy(config->alpine_branch, "edge", sizeof(config->alpine_branch) - 1);
    if (config->recipes_branch[0] == '\0')
        strncpy(config->recipes_branch, "main", sizeof(config->recipes_branch) - 1);

    return FLUX_ERR_NONE;
}


int flux_db_register(const flux_pkg_info_t *info, const char **files, int file_count) {
    char dir[FLUX_MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "/var/lib/flux/installed/%s", info->name);

    mkdir("/var/lib/flux", 0755);
    mkdir("/var/lib/flux/installed", 0755);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        flux_err("failed to create package db entry for %s", info->name);
        return FLUX_ERR_GENERAL;
    }

    char info_path[FLUX_MAX_PATH_LEN + 8];
    snprintf(info_path, sizeof(info_path), "%s/info", dir);
    FILE *f = fopen(info_path, "w");
    if (!f) return FLUX_ERR_GENERAL;
    fprintf(f, "name = %s\n", info->name);
    fprintf(f, "version = %s\n", info->version);
    fprintf(f, "install_date = %s\n", info->install_date);
    fprintf(f, "auto_installed = %d\n", info->auto_installed);
    fprintf(f, "source = %s\n", info->source);
    fclose(f);

    char files_path[FLUX_MAX_PATH_LEN + 8];
    snprintf(files_path, sizeof(files_path), "%s/files", dir);
    f = fopen(files_path, "w");
    if (!f) return FLUX_ERR_GENERAL;
    for (int i = 0; i < file_count; i++)
        fprintf(f, "%s\n", files[i]);
    fclose(f);

    return FLUX_ERR_NONE;
}

int flux_db_is_installed(const char *name) {
    char dir[FLUX_MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "/var/lib/flux/installed/%s", name);
    struct stat st;
    return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

int flux_db_remove(const char *name) {
    char dir[FLUX_MAX_PATH_LEN];
    char info_path[FLUX_MAX_PATH_LEN + 8];
    char files_path[FLUX_MAX_PATH_LEN + 8];

    snprintf(dir, sizeof(dir), "/var/lib/flux/installed/%s", name);
    snprintf(info_path, sizeof(info_path), "/var/lib/flux/installed/%s/info", name);
    snprintf(files_path, sizeof(files_path), "/var/lib/flux/installed/%s/files", name);

    FILE *f = fopen(files_path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            strip_newline(line);
            if (strlen(line) > 0) remove(line);
        }
        fclose(f);
    }

    remove(info_path);
    remove(files_path);
    rmdir(dir);
    return FLUX_ERR_NONE;
}


int flux_native_target(char *out, size_t outlen) {
    FILE *f = popen("gcc -dumpmachine 2>/dev/null", "r");
    if (!f) return FLUX_ERR_GENERAL;
    if (fgets(out, outlen, f) == NULL) {
        pclose(f);
        return FLUX_ERR_GENERAL;
    }
    pclose(f);
    strip_newline(out);
    trim_right(out);
    if (out[0] == '\0') return FLUX_ERR_GENERAL;
    return FLUX_ERR_NONE;
}

int flux_is_archive_name(const char *name) {
    static const char *exts[] = {
        ".tar.gz", ".tgz", ".tar.xz", ".txz", ".tar.bz2", ".tbz2",
        ".tar.zst", ".tzst", ".tar", ".zip", NULL
    };
    size_t len = strlen(name);
    for (int i = 0; exts[i]; i++) {
        size_t elen = strlen(exts[i]);
        if (len > elen && strcmp(name + len - elen, exts[i]) == 0) return 1;
    }
    return 0;
}

static int flux_is_zip_name(const char *name) {
    size_t len = strlen(name);
    return len > 4 && strcmp(name + len - 4, ".zip") == 0;
}

// single-file sources (e.g. a bare .ttf) aren't archives, so they're just copied into dest under their original name instead of extracted
int flux_extract_source(const char *fetched_path, const char *dest) {
    char cmd[896];
    if (!flux_is_archive_name(fetched_path)) {
        const char *base = strrchr(fetched_path, '/');
        base = base ? base + 1 : fetched_path;
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && mkdir -p \"%s\" && cp \"%s\" \"%s/%s\"",
                 dest, dest, fetched_path, dest, base);
        return system(cmd);
    }
    if (flux_is_zip_name(fetched_path)) {
        // unzip has no --strip-components equivalent, so extract to a scratch dir and move the top-level directory's contents up into dest
        snprintf(cmd, sizeof(cmd),
            "rm -rf \"%s\" \"/tmp/flux_zip_extract\" && mkdir -p \"/tmp/flux_zip_extract\" \"%s\""
            " && unzip -q \"%s\" -d \"/tmp/flux_zip_extract\""
            " && mv /tmp/flux_zip_extract/*/* \"%s\"/ 2>/dev/null"
            "; mv /tmp/flux_zip_extract/*/.[!.]* \"%s\"/ 2>/dev/null"
            "; rm -rf \"/tmp/flux_zip_extract\"",
            dest, dest, fetched_path, dest, dest);
        return system(cmd);
    }
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && mkdir -p \"%s\" && tar -xf \"%s\" -C \"%s\" --strip-components=1",
             dest, dest, fetched_path, dest);
    return system(cmd);
}

int flux_cache_key(const char *name, const char *version, const char *cflags, const char *target, char *out, size_t outlen) {
    // hash cflags alone for native builds to preserve existing cache keys; append |target only for cross builds
    char input[FLUX_MAX_CFLAGS_LEN + 64];
    if (target && target[0] != '\0')
        snprintf(input, sizeof(input), "%s|%s", cflags, target);
    else
        snprintf(input, sizeof(input), "%s", cflags);
    char cmd[sizeof(input) + 64];
    snprintf(cmd, sizeof(cmd), "echo -n \"%s\" | sha256sum | cut -c1-6", input);

    FILE *f = popen(cmd, "r");
    if (!f) return FLUX_ERR_GENERAL;

    char hash[16] = {0};
    if (fgets(hash, sizeof(hash), f) == NULL) {
        pclose(f);
        return FLUX_ERR_GENERAL;
    }
    pclose(f);

    strip_newline(hash);
    trim_right(hash);

    snprintf(out, outlen, "%s_%s-%s", name, version, hash);
    return FLUX_ERR_NONE;
}

int flux_cache_lookup_local(const char *key, char *path_out, size_t path_outlen) {
    snprintf(path_out, path_outlen, "/var/cache/flux/%s.tar.zst", key);
    struct stat st;
    if (stat(path_out, &st) == 0) return FLUX_ERR_NONE;
    return FLUX_ERR_NOT_FOUND;
}

int flux_cache_lookup(const char *key, char *path_out, size_t path_outlen) {
    snprintf(path_out, path_outlen, "/var/cache/flux/%s.tar.zst", key);

    struct stat st;
    if (stat(path_out, &st) == 0) return FLUX_ERR_NONE;

    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_NOT_FOUND;
    if (strlen(config.binary_cache_url) == 0) return FLUX_ERR_NOT_FOUND;

    char remote_url[FLUX_MAX_URL_LEN + FLUX_MAX_PATH_LEN];
    snprintf(remote_url, sizeof(remote_url), "%s/packages/%s.tar.zst", config.binary_cache_url, key);
    char check_cmd[FLUX_MAX_URL_LEN + FLUX_MAX_PATH_LEN + 64];
    snprintf(check_cmd, sizeof(check_cmd), "curl -s -o /dev/null -f --head \"%s\"", remote_url);
    if (system(check_cmd) != 0) return FLUX_ERR_NOT_FOUND;

    flux_step("remote cache hit, downloading...");
    system("mkdir -p /var/cache/flux");

    char dl_cmd[FLUX_MAX_URL_LEN + FLUX_MAX_PATH_LEN + 64];

    if (flux_download(remote_url, path_out) != FLUX_ERR_NONE) {
        remove(path_out);
        return FLUX_ERR_NOT_FOUND;
    }

    char sig_path[FLUX_MAX_PATH_LEN + 8];
    snprintf(sig_path, sizeof(sig_path), "%s.minisig", path_out);
    snprintf(dl_cmd, sizeof(dl_cmd), "curl -s -L -o \"%s\" \"%s/packages/%s.tar.zst.minisig\"", sig_path, config.binary_cache_url, key);
    if (system(dl_cmd) != 0) {
        remove(path_out);
        remove(sig_path);
        return FLUX_ERR_NOT_FOUND;
    }

    return FLUX_ERR_NONE;
}

int flux_cache_store(const char *key, const char *destdir, const char *secret_key_path) {
    char archive[FLUX_MAX_PATH_LEN];
    snprintf(archive, sizeof(archive), "/var/cache/flux/%s.tar.zst", key);

    system("mkdir -p /var/cache/flux");

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "tar -C \"%s\" -cf - . | zstd -o \"%s\"", destdir, archive);
    if (system(cmd) != 0) {
        flux_err("failed to create cache archive");
        return FLUX_ERR_CACHE;
    }

    // no signing key on this machine: leave the raw archive for manual transfer/signing elsewhere
    if (access(secret_key_path, R_OK) != 0) {
        flux_ok("cached locally (unsigned, no signing key on this machine): %s", archive);
        return FLUX_ERR_NONE;
    }

    snprintf(cmd, sizeof(cmd), "minisign -Sm \"%s\" -s \"%s\" -W", archive, secret_key_path);
    if (system(cmd) != 0) {
        flux_err("failed to sign cache archive");
        return FLUX_ERR_CACHE;
    }

    // give ownership back to the calling user if running under sudo
    const char *sudo_user = getenv("SUDO_USER");
    if (sudo_user && strlen(sudo_user) > 0) {
        char chown_cmd[FLUX_MAX_PATH_LEN + 512];
        snprintf(chown_cmd, sizeof(chown_cmd), "chown %s:%s \"%s\" \"%s.minisig\"", sudo_user, sudo_user, archive, archive);
        system(chown_cmd);
    }

    flux_ok("cached: %s", archive);
    return FLUX_ERR_NONE;
}

int flux_cache_verify(const char *path, const char *pub_path) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "minisign -Vm \"%s\" -p \"%s\"", path, pub_path);
    if (system(cmd) != 0) {
        flux_err("cache signature verification failed");
        return FLUX_ERR_CACHE;
    }
    return FLUX_ERR_NONE;
}

int flux_db_read_info(const char *name, flux_pkg_info_t *info) {
    char path[FLUX_MAX_PATH_LEN + 8];
    snprintf(path, sizeof(path), "/var/lib/flux/installed/%s/info", name);

    FILE *f = fopen(path, "r");
    if (!f) return FLUX_ERR_NOT_FOUND;

    memset(info, 0, sizeof(*info));
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        strip_newline(line);
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim_right(key);
        val = trim_left(val);

        if (strcmp(key, "name") == 0) strncpy(info->name, val, FLUX_MAX_NAME_LEN - 1);
        if (strcmp(key, "version") == 0) strncpy(info->version, val, FLUX_MAX_VERSION_LEN - 1);
        if (strcmp(key, "install_date") == 0) strncpy(info->install_date, val, sizeof(info->install_date) - 1);
        if (strcmp(key, "auto_installed") == 0) info->auto_installed = atoi(val);
        if (strcmp(key, "source") == 0) strncpy(info->source, val, sizeof(info->source) - 1);
    }
    fclose(f);
    if (info->source[0] == '\0') strncpy(info->source, "kotodama", sizeof(info->source) - 1);
    return FLUX_ERR_NONE;
}

int flux_db_set_auto_installed(const char *name, int auto_installed) {
    flux_pkg_info_t info;
    if (flux_db_read_info(name, &info) != FLUX_ERR_NONE) return FLUX_ERR_NOT_FOUND;
    if (info.auto_installed == auto_installed) return FLUX_ERR_NONE;
    info.auto_installed = auto_installed;

    char path[FLUX_MAX_PATH_LEN + 8];
    snprintf(path, sizeof(path), "/var/lib/flux/installed/%s/info", name);
    FILE *f = fopen(path, "w");
    if (!f) return FLUX_ERR_GENERAL;
    fprintf(f, "name = %s\n", info.name);
    fprintf(f, "version = %s\n", info.version);
    fprintf(f, "install_date = %s\n", info.install_date);
    fprintf(f, "auto_installed = %d\n", info.auto_installed);
    fprintf(f, "source = %s\n", info.source);
    fclose(f);
    return FLUX_ERR_NONE;
}

int flux_db_list_installed(char names[][FLUX_MAX_NAME_LEN], int max, int *count) {
    *count = 0;
    DIR *d = opendir("/var/lib/flux/installed");
    if (!d) return FLUX_ERR_NONE;

    struct dirent *e;
    while ((e = readdir(d)) != NULL && *count < max) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        strncpy(names[*count], e->d_name, FLUX_MAX_NAME_LEN - 1);
        (*count)++;
    }
    closedir(d);
    return FLUX_ERR_NONE;
}

int flux_recipe_depends_on(const char *recipe_name, const char *dep_name, const flux_config_t *config) {
    char koto_path[FLUX_MAX_PATH_LEN * 2 + 16];
    snprintf(koto_path, sizeof(koto_path), "%s/%s/kotodama", config->local_repo_path, recipe_name);

    flux_recipe_t recipe;
    memset(&recipe, 0, sizeof(recipe));
    if (parse_kotodama(&recipe, koto_path) != FLUX_ERR_NONE) return 0;

    for (int i = 0; i < FLUX_MAX_DEPS; i++)
        if (strcmp(recipe.deps[i], dep_name) == 0) return 1;
    for (int i = 0; i < FLUX_MAX_RDEPS; i++)
        if (strcmp(recipe.rdeps[i], dep_name) == 0) return 1;
    return 0;
}

int flux_check_file_conflicts(const char *pkg, const char **paths, int path_count,
                               char *owner_out, size_t owner_outlen,
                               char *colliding_path_out, size_t path_outlen) {
    char names[FLUX_MAX_INSTALL_QUEUE][FLUX_MAX_NAME_LEN];
    int name_count = 0;
    flux_db_list_installed(names, FLUX_MAX_INSTALL_QUEUE, &name_count);

    for (int i = 0; i < name_count; i++) {
        if (strcmp(names[i], pkg) == 0) continue; // own prior files, an upgrade reuses them

        char files_path[FLUX_MAX_PATH_LEN + 8];
        snprintf(files_path, sizeof(files_path), "/var/lib/flux/installed/%s/files", names[i]);
        FILE *f = fopen(files_path, "r");
        if (!f) continue;

        char line[FLUX_MAX_PATH_LEN];
        while (fgets(line, sizeof(line), f)) {
            strip_newline(line);
            if (line[0] == '\0') continue;
            for (int j = 0; j < path_count; j++) {
                if (strcmp(line, paths[j]) == 0) {
                    fclose(f);
                    strncpy(owner_out, names[i], owner_outlen - 1);
                    strncpy(colliding_path_out, line, path_outlen - 1);
                    return FLUX_ERR_GENERAL;
                }
            }
        }
        fclose(f);
    }
    return FLUX_ERR_NONE;
}

int flux_autoremove_orphans(int *removed_count) {
    *removed_count = 0;

    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    // repeat until a pass removes nothing, so a chain of newly-orphaned deps gets cleaned up in one call
    for (int pass = 0; pass < 50; pass++) {
        char names[FLUX_MAX_INSTALL_QUEUE][FLUX_MAX_NAME_LEN];
        int count = 0;
        flux_db_list_installed(names, FLUX_MAX_INSTALL_QUEUE, &count);

        int removed_this_pass = 0;
        for (int i = 0; i < count; i++) {
            flux_pkg_info_t info;
            if (flux_db_read_info(names[i], &info) != FLUX_ERR_NONE) continue;
            if (!info.auto_installed) continue;

            int needed = 0;
            for (int j = 0; j < count; j++) {
                if (j == i) continue;
                if (flux_recipe_depends_on(names[j], names[i], &config)) { needed = 1; break; }
            }
            if (needed) continue;

            flux_step("removing orphaned dependency: %s", names[i]);
            flux_db_remove(names[i]);
            (*removed_count)++;
            removed_this_pass++;
        }
        if (removed_this_pass == 0) break;
    }
    return FLUX_ERR_NONE;
}

int flux_is_kira_pkg(const char *name) {
    return strncmp(name, "kira-", 5) == 0;
}

int flux_run_script(const char *body, const char *env_prefix) {
    if (!body || strlen(body) == 0) return 0;

    system("mkdir -p /tmp/flux-build");
    const char *script_path = "/tmp/flux-build/.flux_script.sh";
    FILE *f = fopen(script_path, "w");
    if (!f) return FLUX_ERR_GENERAL;
    fprintf(f, "#!/bin/sh\nset -e\n%s%s\n", env_prefix ? env_prefix : "", body);
    fclose(f);
    chmod(script_path, 0755);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "sh \"%s\"", script_path);
    int ret = system(cmd);
    remove(script_path);
    return ret;
}

int flux_fetch_latest_git_tag(const char *repo_url, char *out, size_t outlen) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "git ls-remote --tags --refs \"%s\" > /tmp/flux_git_tags_raw 2>/dev/null", repo_url);
    if (system(cmd) != 0) {
        remove("/tmp/flux_git_tags_raw");
        return FLUX_ERR_NETWORK;
    }

    system("sed 's#.*refs/tags/##' /tmp/flux_git_tags_raw | sort -V | tail -1 > /tmp/flux_latest_tag");
    remove("/tmp/flux_git_tags_raw");

    FILE *f = fopen("/tmp/flux_latest_tag", "r");
    if (!f) return FLUX_ERR_NETWORK;
    if (!fgets(out, outlen, f)) {
        fclose(f);
        remove("/tmp/flux_latest_tag");
        return FLUX_ERR_NOT_FOUND;
    }
    fclose(f);
    remove("/tmp/flux_latest_tag");
    strip_newline(out);
    if (strlen(out) == 0) return FLUX_ERR_NOT_FOUND;
    return FLUX_ERR_NONE;
}

int flux_colors_enabled(void) {
    static int checked = 0;
    static int enabled = 0;
    if (!checked) {
        enabled = isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL;
        checked = 1;
    }
    return enabled;
}

// glyph-prefixed, color-wrapped line with a trailing newline already included
static void flux_vglyph(FILE *stream, const char *color, const char *glyph, const char *fmt, va_list ap) {
    if (flux_colors_enabled()) fprintf(stream, "%s%s ", color, glyph);
    else fprintf(stream, "%s ", glyph);
    vfprintf(stream, fmt, ap);
    if (flux_colors_enabled()) fprintf(stream, "\033[0m");
    fputc('\n', stream);
}

void flux_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    flux_vglyph(stdout, "\033[36m", "\xe2\x86\x92", fmt, ap); // → cyan
    va_end(ap);
}

void flux_ok(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    flux_vglyph(stdout, "\033[32m", "\xe2\x9c\x93", fmt, ap); // ✓ green
    va_end(ap);
}

void flux_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    flux_vglyph(stdout, "\033[33m", "!", fmt, ap); // yellow
    va_end(ap);
}

void flux_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    flux_vglyph(stderr, "\033[31m", "\xe2\x9c\x97", fmt, ap); // ✗ red
    va_end(ap);
}

void flux_action(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (flux_colors_enabled()) printf("\033[1;35m");
    vprintf(fmt, ap);
    if (flux_colors_enabled()) printf("\033[0m");
    printf("\n");
    va_end(ap);
}

void flux_step(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (flux_colors_enabled()) printf("  \033[36m\xe2\x80\xa2\033[0m "); // • dim cyan bullet
    else printf("  * ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

int flux_term_width(void) {
    struct winsize w;
    if (isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
    return 72;
}

void flux_rule(void) {
    int width = flux_term_width();
    if (flux_colors_enabled()) printf("\033[2m");
    for (int i = 0; i < width; i++) fputs("\xe2\x94\x80", stdout); // ─
    if (flux_colors_enabled()) printf("\033[0m");
    printf("\n");
}

void flux_print_table(const char *title, const flux_table_row_t *rows, int count) {
    size_t w1 = 0, w2 = 0;
    for (int i = 0; i < count; i++) {
        size_t l1 = strlen(rows[i].col1), l2 = strlen(rows[i].col2);
        if (l1 > w1) w1 = l1;
        if (l2 > w2) w2 = l2;
    }
    if (title) {
        if (flux_colors_enabled()) printf("\033[1m");
        printf("%s\n", title);
        if (flux_colors_enabled()) printf("\033[0m");
    }
    flux_rule();
    for (int i = 0; i < count; i++)
        printf("  %-*s  %-*s\n", (int)w1, rows[i].col1, (int)w2, rows[i].col2);
    flux_rule();
}

double flux_now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void format_bytes(double bytes, char *out, size_t outlen) {
    static const char *units[] = {"B", "KB", "MB", "GB"};
    int u = 0;
    double v = bytes;
    while (v >= 1024.0 && u < 3) { v /= 1024.0; u++; }
    snprintf(out, outlen, "%.1f%s", v, units[u]);
}

// best-effort HEAD request; -1 if the server doesn't report a size
static long fetch_content_length(const char *url) {
    char cmd[FLUX_MAX_URL_LEN + 128];
    snprintf(cmd, sizeof(cmd),
        "curl -sIL --max-time 15 \"%s\" 2>/dev/null | tr -d '\\r'"
        " | awk -F': ' 'tolower($1)==\"content-length\"{v=$2} END{print v}'", url);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    char buf[32] = {0};
    long len = -1;
    if (fgets(buf, sizeof(buf), f)) {
        char *end;
        long v = strtol(buf, &end, 10);
        if (end != buf && v > 0) len = v;
    }
    pclose(f);
    return len;
}

// downloads url to dest via a forked, silenced curl, drawing a Kira-purple
// progress bar in its place instead of curl's own ASCII meter - falls back
// to a plain byte counter when the server doesn't report a Content-Length,
// and to two static lines (no live redraw) when stdout isn't a real tty
int flux_download(const char *url, const char *dest) {
    long total = fetch_content_length(url);
    const char *base = strrchr(dest, '/');
    base = base ? base + 1 : dest;
    int tty = isatty(STDOUT_FILENO);
    const char *purple = flux_colors_enabled() ? "\033[38;2;170;0;255m" : "";
    const char *reset = flux_colors_enabled() ? "\033[0m" : "";

    if (!tty) printf("  \xe2\x86\x93 %s\n", base); // ↓
    remove(dest); // a stale leftover at dest would otherwise flash 100% for one frame before curl truncates it

    pid_t pid = fork();
    if (pid < 0) return FLUX_ERR_GENERAL;

    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
        execlp("curl", "curl", "-fL", "--retry", "3", "--max-time", "3600", "-o", dest, url, (char *)NULL);
        _exit(127);
    }

    struct stat st;
    double t_start = flux_now_seconds();
    int status = 0;
    pid_t w;
    const int bar_w = 30;

    while ((w = waitpid(pid, &status, WNOHANG)) == 0) {
        if (tty) {
            long cur = (stat(dest, &st) == 0) ? (long)st.st_size : 0;
            double elapsed = flux_now_seconds() - t_start;
            char cur_s[16], rate_s[16];
            format_bytes((double)cur, cur_s, sizeof(cur_s));
            format_bytes(elapsed > 0.1 ? (double)cur / elapsed : 0, rate_s, sizeof(rate_s));

            if (total > 0) {
                int pct = (int)((cur * 100) / total);
                if (pct > 100) pct = 100;
                int filled = pct * bar_w / 100;
                char tot_s[16];
                format_bytes((double)total, tot_s, sizeof(tot_s));
                printf("\r  %s[", purple);
                for (int i = 0; i < bar_w; i++)
                    fputs(i < filled ? "\xe2\x96\x88" : "\xe2\x96\x91", stdout); // █ / ░
                printf("]%s %3d%%  %s/%s  %s/s\033[K", reset, pct, cur_s, tot_s, rate_s);
            } else {
                printf("\r  %s\xe2\x86\x93%s %s  %s  %s/s\033[K", purple, reset, base, cur_s, rate_s);
            }
            fflush(stdout);
        }
        struct timespec ts = {0, 150000000L};
        nanosleep(&ts, NULL);
    }
    if (w < 0) return FLUX_ERR_GENERAL;

    if (tty) printf("\r\033[K");

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return FLUX_ERR_NETWORK;
    return FLUX_ERR_NONE;
}

// downloads several files sequentially behind one shared progress bar (bytes
// done across the whole batch / bytes total across the whole batch), instead
// of flux_download's one-bar-per-file - falls back to a plain per-file
// counter if any server in the batch doesn't report a Content-Length
int flux_download_batch(const flux_download_item_t *items, int count) {
    if (count <= 0) return FLUX_ERR_NONE;
    if (count == 1) return flux_download(items[0].url, items[0].dest);

    long totals[FLUX_MAX_INSTALL_QUEUE];
    long grand_total = 0;
    int have_all_sizes = 1;
    for (int i = 0; i < count && i < FLUX_MAX_INSTALL_QUEUE; i++) {
        totals[i] = fetch_content_length(items[i].url);
        if (totals[i] < 0) have_all_sizes = 0;
        else grand_total += totals[i];
    }

    int tty = isatty(STDOUT_FILENO);
    const char *purple = flux_colors_enabled() ? "\033[38;2;170;0;255m" : "";
    const char *reset = flux_colors_enabled() ? "\033[0m" : "";
    const int bar_w = 30;
    long done_bytes = 0;

    printf("Downloading %d packages...\n", count);
    for (int i = 0; i < count; i++) {
        const char *base = strrchr(items[i].dest, '/');
        base = base ? base + 1 : items[i].dest;
        if (!tty) printf("  \xe2\x86\x93 %s (%d/%d)\n", base, i + 1, count); // ↓
        remove(items[i].dest);

        pid_t pid = fork();
        if (pid < 0) return FLUX_ERR_GENERAL;

        if (pid == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
            execlp("curl", "curl", "-fL", "--retry", "3", "--max-time", "3600", "-o", items[i].dest, items[i].url, (char *)NULL);
            _exit(127);
        }

        struct stat st;
        double t_start = flux_now_seconds();
        int status = 0;
        pid_t w;

        while ((w = waitpid(pid, &status, WNOHANG)) == 0) {
            if (tty) {
                long cur = (stat(items[i].dest, &st) == 0) ? (long)st.st_size : 0;
                double elapsed = flux_now_seconds() - t_start;
                char cur_s[16], rate_s[16];
                format_bytes((double)(done_bytes + cur), cur_s, sizeof(cur_s));
                format_bytes(elapsed > 0.1 ? (double)cur / elapsed : 0, rate_s, sizeof(rate_s));

                if (have_all_sizes && grand_total > 0) {
                    int pct = (int)(((done_bytes + cur) * 100) / grand_total);
                    if (pct > 100) pct = 100;
                    int filled = pct * bar_w / 100;
                    char tot_s[16];
                    format_bytes((double)grand_total, tot_s, sizeof(tot_s));
                    printf("\r  %s[", purple);
                    for (int b = 0; b < bar_w; b++)
                        fputs(b < filled ? "\xe2\x96\x88" : "\xe2\x96\x91", stdout); // █ / ░
                    printf("]%s %3d%%  (%d/%d)  %s/%s  %s/s\033[K", reset, pct, i + 1, count, cur_s, tot_s, rate_s);
                } else {
                    printf("\r  %s\xe2\x86\x93%s (%d/%d) %s  %s  %s/s\033[K", purple, reset, i + 1, count, base, cur_s, rate_s);
                }
                fflush(stdout);
            }
            struct timespec ts = {0, 150000000L};
            nanosleep(&ts, NULL);
        }
        if (w < 0) return FLUX_ERR_GENERAL;

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            if (tty) printf("\r\033[K");
            flux_err("failed to download %s", base);
            return FLUX_ERR_NETWORK;
        }

        struct stat fst;
        done_bytes += (stat(items[i].dest, &fst) == 0) ? (long)fst.st_size : (totals[i] > 0 ? totals[i] : 0);
    }
    if (tty) printf("\r\033[K");
    return FLUX_ERR_NONE;
}