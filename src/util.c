#include <stdio.h>
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
    char info_path[FLUX_MAX_PATH_LEN];
    snprintf(info_path, sizeof(info_path), "%s/info", dir);
    FILE *f = fopen(info_path, "w");
    if (!f) return FLUX_ERR_GENERAL;
    fprintf(f, "name = %s\n", info->name);
    fprintf(f, "version = %s\n", info->version);
    fprintf(f, "install_date = %s\n", info->install_date);
    fprintf(f, "auto_installed = %d\n", info->auto_installed);
    fclose(f);

    // write files list
    char files_path[FLUX_MAX_PATH_LEN];
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
    char info_path[FLUX_MAX_PATH_LEN];
    char files_path[FLUX_MAX_PATH_LEN];
    char dir[FLUX_MAX_PATH_LEN];

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