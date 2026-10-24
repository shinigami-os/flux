#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
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

int alpine_name_is_safe(const char *s) {
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '.' || c == '_' || c == '+' || c == '-' || c == ':' || c == '@')) return 0;
    }
    return 1;
}

int alpine_index_url(const flux_config_t *config, const char *repo, const char *arch, char *out, size_t outlen) {
    snprintf(out, outlen, "%s/%s/%s/%s/APKINDEX.tar.gz", config->alpine_mirror_url, config->alpine_branch, repo, arch);
    return FLUX_ERR_NONE;
}

void alpine_index_cache_path(const char *repo, const char *arch, char *out, size_t outlen) {
    snprintf(out, outlen, "/var/cache/flux/alpine/%s-%s-APKINDEX.tar.gz", repo, arch);
}

int alpine_index_fetch(const flux_config_t *config, const char *repo, const char *arch, char *path_out, size_t path_outlen) {
    char url[FLUX_MAX_URL_LEN * 2];
    if (alpine_index_url(config, repo, arch, url, sizeof(url)) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    system("mkdir -p /var/cache/flux/alpine");
    alpine_index_cache_path(repo, arch, path_out, path_outlen);

    flux_step("fetching Alpine %s index (%s/%s)...", arch, config->alpine_branch, repo);
    return flux_download(url, path_out);
}

// signed like a .apk control member: 2 gzip members (signature, index tarball) - refusing an unverified index also blocks a malicious mirror steering name/version resolution into shell input
static int verify_and_extract_index(const char *tar_gz_path, char *out_index_path) {
    long offsets[4];
    int member_count = 0;
    if (alpine_gzip_find_members(tar_gz_path, offsets, 4, &member_count) != FLUX_ERR_NONE || member_count != 2) {
        flux_err("APKINDEX does not have the expected signed format (found %d members)", member_count);
        return FLUX_ERR_SOURCE;
    }

    struct stat st;
    if (stat(tar_gz_path, &st) != 0) return FLUX_ERR_SOURCE;

    system("mkdir -p /tmp/flux-build");
    const char *sig_path = "/tmp/flux-build/apkindex-sig.tar.gz";
    const char *data_path = "/tmp/flux-build/apkindex-data.tar.gz";
    if (alpine_gzip_extract_range(tar_gz_path, offsets[0], offsets[1] - offsets[0], sig_path) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;
    if (alpine_gzip_extract_range(tar_gz_path, offsets[1], (long)st.st_size - offsets[1], data_path) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    if (alpine_verify_signature(data_path, sig_path, ALPINE_KEYS_DIR) != FLUX_ERR_NONE) {
        flux_err("APKINDEX signature verification failed, refusing to trust it");
        remove(sig_path);
        remove(data_path);
        return FLUX_ERR_CACHE;
    }

    const char *destdir = "/tmp/flux-alpine-index-extract";
    char cmd[768];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && mkdir -p \"%s\" && tar -xzf \"%s\" -C \"%s\" APKINDEX", destdir, destdir, data_path, destdir);
    int ok = (system(cmd) == 0);
    remove(sig_path);
    remove(data_path);
    if (!ok) return FLUX_ERR_SOURCE;

    snprintf(out_index_path, 300, "%s/APKINDEX", destdir);
    return FLUX_ERR_NONE;
}

// sorting only - never used for bsearch's own comparisons. The tie-break matters for making
// the sort deterministic across a duplicate name, but it would break searching outright: once
// strcmp reports a name match (c == 0), falling through to compare the search key's own (always
// distinct, unrelated) address against the candidate's would make the comparator report "not
// equal" for literally every match, since the two are never the same object - bsearch would then
// never see the 0 it needs to report found, and every lookup would silently return NULL.
static int pkg_ptr_sort_cmp(const void *a, const void *b) {
    const alpine_pkg_t *const *pa = a;
    const alpine_pkg_t *const *pb = b;
    int c = strcmp((*pa)->name, (*pb)->name);
    if (c != 0) return c;
    // stable tie-break for a duplicate name: both pointers are into the same contiguous
    // pkgs[] array, so comparing them directly recovers original (file) order exactly
    return (*pa) < (*pb) ? -1 : ((*pa) > (*pb) ? 1 : 0);
}

// bsearch only - compares by name alone. sorted[] is already deduplicated by the time anything
// searches it, so there's never a tie left to break here.
static int pkg_ptr_name_cmp(const void *a, const void *b) {
    const alpine_pkg_t *const *pa = a;
    const alpine_pkg_t *const *pb = b;
    return strcmp((*pa)->name, (*pb)->name);
}

int alpine_index_load(const char *tar_gz_path, alpine_index_t *index) {
    memset(index, 0, sizeof(*index));

    char idx_path[300];
    if (verify_and_extract_index(tar_gz_path, idx_path) != FLUX_ERR_NONE) return FLUX_ERR_SOURCE;

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
    system("rm -rf /tmp/flux-alpine-index-extract");

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
            // reject a stanza with an unsafe name/version rather than let it become an addressable package later
            if (have_pkg && alpine_name_is_safe(cur.name) && alpine_name_is_safe(cur.version) && count < ALPINE_MAX_INDEX_PKGS)
                pkgs[count++] = cur;
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
    if (have_pkg && alpine_name_is_safe(cur.name) && alpine_name_is_safe(cur.version) && count < ALPINE_MAX_INDEX_PKGS)
        pkgs[count++] = cur;

    index->raw_text = buf;
    index->pkgs = pkgs;
    index->count = count;

    index->sorted = malloc(sizeof(alpine_pkg_t *) * (size_t)count);
    index->sorted_count = 0;
    if (index->sorted) {
        for (int i = 0; i < count; i++) index->sorted[i] = &pkgs[i];
        qsort(index->sorted, (size_t)count, sizeof(alpine_pkg_t *), pkg_ptr_sort_cmp);
        int unique = 0;
        for (int i = 0; i < count; i++) {
            if (unique > 0 && strcmp(index->sorted[unique - 1]->name, index->sorted[i]->name) == 0) continue;
            index->sorted[unique++] = index->sorted[i];
        }
        index->sorted_count = unique;
    }
    return FLUX_ERR_NONE;
}

void alpine_index_free(alpine_index_t *index) {
    free(index->pkgs);
    free(index->raw_text);
    free(index->sorted);
    index->pkgs = NULL;
    index->raw_text = NULL;
    index->sorted = NULL;
    index->count = 0;
    index->sorted_count = 0;
}

alpine_pkg_t *alpine_index_find(alpine_index_t *index, const char *name) {
    if (!index->sorted || index->sorted_count == 0) return NULL;

    alpine_pkg_t key;
    strncpy(key.name, name, sizeof(key.name) - 1);
    key.name[sizeof(key.name) - 1] = '\0';
    alpine_pkg_t *keyptr = &key;

    // the key's own address has no meaningful relationship to pkgs[]'s layout, so the tie-break
    // half of pkg_ptr_name_cmp is never exercised here - fine, since sorted[] is already
    // deduplicated and every name in it is unique, there's nothing left to tie-break between
    alpine_pkg_t **found = bsearch(&keyptr, index->sorted, (size_t)index->sorted_count,
                                    sizeof(alpine_pkg_t *), pkg_ptr_name_cmp);
    return found ? *found : NULL;
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
