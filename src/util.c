#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/util.h"
#include "../include/parser.h"
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
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
        fprintf(stderr, "flux: cannot open /etc/flux/flux.conf\n");
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
    }

    fclose(f);
    return FLUX_ERR_NONE;
}


int flux_db_register(const flux_pkg_info_t *info, const char **files, int file_count) {
    char dir[FLUX_MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "/var/lib/flux/installed/%s", info->name);

    mkdir("/var/lib/flux", 0755);
    mkdir("/var/lib/flux/installed", 0755);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "flux: failed to create package db entry for %s\n", info->name);
        return FLUX_ERR_GENERAL;
    }

    // write info file
    char info_path[FLUX_MAX_PATH_LEN + 8];
    snprintf(info_path, sizeof(info_path), "%s/info", dir);
    FILE *f = fopen(info_path, "w");
    if (!f) return FLUX_ERR_GENERAL;
    fprintf(f, "name = %s\n", info->name);
    fprintf(f, "version = %s\n", info->version);
    fprintf(f, "install_date = %s\n", info->install_date);
    fprintf(f, "auto_installed = %d\n", info->auto_installed);
    fclose(f);

    // write files list
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

    // read and delete each installed file
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

// extracts a fetched source into dest, auto-detecting compression and stripping
// the top dir for real archives. Single-file sources (e.g. a bare .ttf) aren't
// archives at all, so they're just copied into dest under their original name.
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
        // unzip has no --strip-components equivalent, so extract to a scratch
        // dir and move the single top-level directory's contents up into dest
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
    // hash cflags alone for native builds (preserves existing cache keys),
    // append |target for cross builds so they get a distinct key
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

    // strip any trailing whitespace/newline from hash
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

    // check local cache first
    struct stat st;
    if (stat(path_out, &st) == 0) return FLUX_ERR_NONE;

    // local miss: try remote cache
    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_NOT_FOUND;
    if (strlen(config.binary_cache_url) == 0) return FLUX_ERR_NOT_FOUND;

    // check remote cache by attempting a HEAD request
    char remote_url[FLUX_MAX_URL_LEN + FLUX_MAX_PATH_LEN];
    snprintf(remote_url, sizeof(remote_url), "%s/packages/%s.tar.zst", config.binary_cache_url, key);
    char check_cmd[FLUX_MAX_URL_LEN + FLUX_MAX_PATH_LEN + 64];
    snprintf(check_cmd, sizeof(check_cmd), "curl -s -o /dev/null -f --head \"%s\"", remote_url);
    if (system(check_cmd) != 0) return FLUX_ERR_NOT_FOUND;

    printf("[flux] remote cache hit, downloading...\n");
    system("mkdir -p /var/cache/flux");

    char dl_cmd[FLUX_MAX_URL_LEN + FLUX_MAX_PATH_LEN + 64];

    // download archive
    snprintf(dl_cmd, sizeof(dl_cmd), "curl -L --max-time 3600 --retry 3 -o \"%s\" \"%s/packages/%s.tar.zst\"", path_out, config.binary_cache_url, key);
    if (system(dl_cmd) != 0) {
        remove(path_out);
        return FLUX_ERR_NOT_FOUND;
    }

    // download signature
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

    // package destdir into tar.zst
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "tar -C \"%s\" -cf - . | zstd -o \"%s\"", destdir, archive);
    if (system(cmd) != 0) {
        fprintf(stderr, "flux: failed to create cache archive\n");
        return FLUX_ERR_CACHE;
    }

    // no signing key on this machine: leave the raw archive for manual transfer/signing elsewhere
    if (access(secret_key_path, R_OK) != 0) {
        printf("[flux] cached locally (unsigned, no signing key on this machine): %s\n", archive);
        return FLUX_ERR_NONE;
    }

    // sign with minisign
    snprintf(cmd, sizeof(cmd), "minisign -Sm \"%s\" -s \"%s\" -W", archive, secret_key_path);
    if (system(cmd) != 0) {
        fprintf(stderr, "flux: failed to sign cache archive\n");
        return FLUX_ERR_CACHE;
    }

    // give ownership back to the calling user if running under sudo
    const char *sudo_user = getenv("SUDO_USER");
    if (sudo_user && strlen(sudo_user) > 0) {
        char chown_cmd[FLUX_MAX_PATH_LEN + 512];
        snprintf(chown_cmd, sizeof(chown_cmd), "chown %s:%s \"%s\" \"%s.minisig\"", sudo_user, sudo_user, archive, archive);
        system(chown_cmd);
    }

    printf("[flux] cached: %s\n", archive);
    return FLUX_ERR_NONE;
}

int flux_cache_verify(const char *path, const char *pub_path) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "minisign -Vm \"%s\" -p \"%s\"", path, pub_path);
    if (system(cmd) != 0) {
        fprintf(stderr, "flux: cache signature verification failed\n");
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
    }
    fclose(f);
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

int flux_autoremove_orphans(int *removed_count) {
    *removed_count = 0;

    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    // repeat until a full pass removes nothing, so a chain of
    // now-orphaned auto-installed deps gets cleaned up in one call
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

            printf("[flux] removing orphaned dependency: %s\n", names[i]);
            flux_db_remove(names[i]);
            (*removed_count)++;
            removed_this_pass++;
        }
        if (removed_this_pass == 0) break;
    }
    return FLUX_ERR_NONE;
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

static void flux_vlog(FILE *stream, const char *color, const char *fmt, va_list ap) {
    if (flux_colors_enabled()) fprintf(stream, "%s", color);
    vfprintf(stream, fmt, ap);
    if (flux_colors_enabled()) fprintf(stream, "\033[0m");
}

void flux_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    flux_vlog(stdout, "\033[36m", fmt, ap);
    va_end(ap);
}

void flux_ok(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    flux_vlog(stdout, "\033[32m", fmt, ap);
    va_end(ap);
}

void flux_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    flux_vlog(stdout, "\033[33m", fmt, ap);
    va_end(ap);
}

void flux_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    flux_vlog(stderr, "\033[31m", fmt, ap);
    va_end(ap);
}