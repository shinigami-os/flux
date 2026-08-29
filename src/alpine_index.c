#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../include/alpine.h"
#include "../include/util.h"

int alpine_arch_from_target(const char *package_target, char *out, size_t outlen) {
    const char *dash = strchr(package_target, '-');
    size_t len = dash ? (size_t)(dash - package_target) : strlen(package_target);
    if (len >= outlen) len = outlen - 1;
    memcpy(out, package_target, len);
    out[len] = '\0';
    return out[0] ? FLUX_ERR_NONE : FLUX_ERR_GENERAL;
}

int alpine_index_url(const flux_config_t *config, const char *repo, const char *arch, char *out, size_t outlen) {
    snprintf(out, outlen, "%s/%s/%s/%s/APKINDEX.tar.gz", config->alpine_mirror_url, config->alpine_branch, repo, arch);
    return FLUX_ERR_NONE;
}

int alpine_index_fetch(const flux_config_t *config, const char *repo, const char *arch, char *path_out, size_t path_outlen) {
    char url[FLUX_MAX_URL_LEN * 2];
    if (alpine_index_url(config, repo, arch, url, sizeof(url)) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    system("mkdir -p /var/cache/flux/alpine");
    snprintf(path_out, path_outlen, "/var/cache/flux/alpine/%s-%s-APKINDEX.tar.gz", repo, arch);

    flux_step("fetching Alpine %s index (%s/%s)...", arch, config->alpine_branch, repo);
    return flux_download(url, path_out);
}

int alpine_index_load(const char *tar_gz_path, alpine_index_t *index) {
    memset(index, 0, sizeof(*index));

    const char *destdir = "/tmp/flux-alpine-index-extract";
    char cmd[768];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && mkdir -p \"%s\" && tar -xzf \"%s\" -C \"%s\" APKINDEX", destdir, destdir, tar_gz_path, destdir);
    if (system(cmd) != 0) return FLUX_ERR_SOURCE;

    char idx_path[300];
    snprintf(idx_path, sizeof(idx_path), "%s/APKINDEX", destdir);
    FILE *f = fopen(idx_path, "rb");
    if (!f) return FLUX_ERR_SOURCE;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return FLUX_ERR_SOURCE; }

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return FLUX_ERR_GENERAL; }
    size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[rd] = '\0';

    char rmcmd[320];
    snprintf(rmcmd, sizeof(rmcmd), "rm -rf \"%s\"", destdir);
    system(rmcmd);

    alpine_pkg_t *pkgs = malloc(sizeof(alpine_pkg_t) * ALPINE_MAX_INDEX_PKGS);
    if (!pkgs) { free(buf); return FLUX_ERR_GENERAL; }

    alpine_pkg_t cur;
    memset(&cur, 0, sizeof(cur));
    int have_pkg = 0;
    int count = 0;

    char *line = buf;
    while (line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (*line == '\0') {
            if (have_pkg && count < ALPINE_MAX_INDEX_PKGS) pkgs[count++] = cur;
            memset(&cur, 0, sizeof(cur));
            have_pkg = 0;
        } else if (line[1] == ':') {
            char key = line[0];
            char *val = line + 2;
            switch (key) {
                case 'P': strncpy(cur.name, val, ALPINE_MAX_NAME_LEN - 1); have_pkg = 1; break;
                case 'V': strncpy(cur.version, val, ALPINE_MAX_VERSION_LEN - 1); break;
                case 'A': strncpy(cur.arch, val, ALPINE_MAX_ARCH_LEN - 1); break;
                case 'T': strncpy(cur.description, val, ALPINE_MAX_DESC_LEN - 1); break;
                case 'U': strncpy(cur.url, val, ALPINE_MAX_URL_LEN - 1); break;
                case 'L': strncpy(cur.license, val, ALPINE_MAX_LICENSE_LEN - 1); break;
                case 'o': strncpy(cur.origin, val, ALPINE_MAX_ORIGIN_LEN - 1); break;
                case 'C': strncpy(cur.checksum, val, ALPINE_MAX_CHECKSUM_LEN - 1); break;
                case 'S': cur.size = atol(val); break;
                case 'I': cur.installed_size = atol(val); break;
                case 'D': cur.depends_raw = val; break;
                case 'p': cur.provides_raw = val; break;
                default: break;
            }
        }

        line = nl ? nl + 1 : NULL;
    }
    if (have_pkg && count < ALPINE_MAX_INDEX_PKGS) pkgs[count++] = cur;

    index->raw_text = buf;
    index->pkgs = pkgs;
    index->count = count;
    return FLUX_ERR_NONE;
}

void alpine_index_free(alpine_index_t *index) {
    free(index->pkgs);
    free(index->raw_text);
    index->pkgs = NULL;
    index->raw_text = NULL;
    index->count = 0;
}

alpine_pkg_t *alpine_index_find(alpine_index_t *index, const char *name) {
    for (int i = 0; i < index->count; i++)
        if (strcmp(index->pkgs[i].name, name) == 0) return &index->pkgs[i];
    return NULL;
}

int alpine_parse_dep_line(const char *raw, alpine_dep_t *out, int max, int *count) {
    *count = 0;
    if (!raw || *raw == '\0') return FLUX_ERR_NONE;

    char *tmp = strdup(raw);
    if (!tmp) return FLUX_ERR_GENERAL;

    char *tok = strtok(tmp, " ");
    while (tok && *count < max) {
        strncpy(out[*count].token, tok, ALPINE_MAX_DEP_TOKEN_LEN - 1);
        (*count)++;
        tok = strtok(NULL, " ");
    }
    free(tmp);
    return FLUX_ERR_NONE;
}
