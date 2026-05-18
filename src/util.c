#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/util.h"
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

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
    }

    fclose(f);
    return FLUX_ERR_NONE;
}


int flux_db_register(const flux_pkg_info_t *info, const char **files, int file_count) {
    char dir[FLUX_MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "/var/lib/flux/installed/%s", info->name);

    if (mkdir(dir, 0755) != 0) {
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


int flux_cache_key(const char *name, const char *version, const char *cflags, char *out, size_t outlen) {
    // hash the cflags string
    char cmd[FLUX_MAX_CFLAGS_LEN + 64];
    snprintf(cmd, sizeof(cmd), "echo -n \"%s\" | sha256sum | cut -c1-6", cflags);

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

    snprintf(out, outlen, "%s-%s-%s", name, version, hash);
    return FLUX_ERR_NONE;
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
    snprintf(dl_cmd, sizeof(dl_cmd), "curl -s -L -o \"%s\" \"%s/packages/%s.tar.zst\"", path_out, config.binary_cache_url, key);
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
    snprintf(cmd, sizeof(cmd), "tar -C \"%s\" -I zstd -cf \"%s\" .", destdir, archive);
    if (system(cmd) != 0) {
        fprintf(stderr, "flux: failed to create cache archive\n");
        return FLUX_ERR_CACHE;
    }

    // sign with minisign
    snprintf(cmd, sizeof(cmd), "minisign -Sm \"%s\" -s \"%s\" -W", archive, secret_key_path);
    if (system(cmd) != 0) {
        fprintf(stderr, "flux: failed to sign cache archive\n");
        return FLUX_ERR_CACHE;
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