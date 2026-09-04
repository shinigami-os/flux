#ifndef ALPINE_H
#define ALPINE_H

#include "flux.h"
#include <stddef.h>

#define ALPINE_MAX_NAME_LEN       64
#define ALPINE_MAX_VERSION_LEN    48
#define ALPINE_MAX_DESC_LEN       256
#define ALPINE_MAX_URL_LEN        256
#define ALPINE_MAX_LICENSE_LEN    64
#define ALPINE_MAX_ARCH_LEN       16
#define ALPINE_MAX_ORIGIN_LEN     64
#define ALPINE_MAX_CHECKSUM_LEN   96
#define ALPINE_MAX_DEP_TOKEN_LEN  96
// sized for real outliers (some split packages carry 100+ deps/provides), not the common case
#define ALPINE_MAX_TOKENS_PER_LINE 320

typedef struct {
    char token[ALPINE_MAX_DEP_TOKEN_LEN];
} alpine_dep_t;

// depends_raw/provides_raw point into the owning alpine_index_t's raw_text, not copied - see alpine_parse_dep_line
typedef struct {
    char name[ALPINE_MAX_NAME_LEN];
    char version[ALPINE_MAX_VERSION_LEN];
    char arch[ALPINE_MAX_ARCH_LEN];
    char description[ALPINE_MAX_DESC_LEN];
    char url[ALPINE_MAX_URL_LEN];
    char license[ALPINE_MAX_LICENSE_LEN];
    char origin[ALPINE_MAX_ORIGIN_LEN];
    char checksum[ALPINE_MAX_CHECKSUM_LEN];
    long size;
    long installed_size;
    const char *depends_raw;
    const char *provides_raw;
} alpine_pkg_t;

#define ALPINE_MAX_INDEX_PKGS 24000 // edge main+community currently well under this

typedef struct {
    char *raw_text;
    alpine_pkg_t *pkgs;
    int count;
} alpine_index_t;

int alpine_arch_from_target(const char *package_target, char *out, size_t outlen);
int alpine_index_url(const flux_config_t *config, const char *repo, const char *arch, char *out, size_t outlen);
void alpine_index_cache_path(const char *repo, const char *arch, char *out, size_t outlen);
int alpine_index_fetch(const flux_config_t *config, const char *repo, const char *arch, char *path_out, size_t path_outlen);
// fetches+verifies the signed index, then parses it - see alpine_verify_signature
int alpine_index_load(const char *tar_gz_path, alpine_index_t *index);
void alpine_index_free(alpine_index_t *index);
alpine_pkg_t *alpine_index_find(alpine_index_t *index, const char *name);
int alpine_parse_dep_line(const char *raw, alpine_dep_t *out, int max, int *count);
// true if every char in s is safe to embed in a shell command string (name/version/key-filename validation)
int alpine_name_is_safe(const char *s);

// both .apk and APKINDEX.tar.gz are concatenated independently-valid gzip streams (signature + payload)
int alpine_gzip_find_members(const char *path, long *offsets, int max_offsets, int *count);
int alpine_gzip_extract_range(const char *path, long start, long len, const char *out_path);

int alpine_apk_url(const flux_config_t *config, const char *repo, const char *arch, const char *name, const char *version, char *out, size_t outlen);
int alpine_apk_download(const flux_config_t *config, const char *repo, const char *arch, const char *name, const char *version, char *path_out, size_t path_outlen);
// splits a .apk into its 3 members: signature, control (.PKGINFO + scripts), data (installable files)
int alpine_apk_split_members(const char *apk_path, char *sig_path_out, char *control_path_out, char *data_path_out, size_t path_outlen);
int alpine_apk_extract(const char *member_tar_gz_path, const char *destdir);

// main + community held for one dependency walk's lifetime, so N dependencies don't refetch/reparse the index N times
typedef struct {
    alpine_index_t main;
    alpine_index_t community;
} alpine_repos_t;

// fetches+verifies+caches both indexes from the network - only `flux update` should call this
int alpine_repos_sync(const flux_config_t *config, const char *arch, alpine_repos_t *repos);
// loads whatever `flux update` last cached, no network - what `flux install`/`flux search`/etc use, fails with a "run flux update" hint if nothing is cached yet
int alpine_repos_load(const flux_config_t *config, const char *arch, alpine_repos_t *repos);
void alpine_repos_free(alpine_repos_t *repos);
const alpine_pkg_t *alpine_repos_find_by_name(const alpine_repos_t *repos, const char *name, const char **out_repo);
const alpine_pkg_t *alpine_repos_find_provider(const alpine_repos_t *repos, const char *capability_token, const char **out_repo);

#define ALPINE_MAX_RESOLVED_DEPS 128

// resolves one package's D: tokens to concrete package names (so:/cmd:/pc: capabilities included); "!pkg" conflict tokens go to conflicts_out instead
int alpine_resolve_deps(const alpine_repos_t *repos, const alpine_pkg_t *pkg,
                         char names_out[][ALPINE_MAX_NAME_LEN], int max_names, int *names_count,
                         char conflicts_out[][ALPINE_MAX_NAME_LEN], int max_conflicts, int *conflicts_count);

#define ALPINE_KEYS_DIR "/etc/flux/alpine-keys"

#define ALPINE_TRIGGER_COUNT 3
// ".pre-install"/".post-install"/".trigger", in run order, for index in [0, ALPINE_TRIGGER_COUNT)
const char *alpine_trigger_script_name(int index);
int alpine_has_triggers(const char *control_extract_dir);
// FLUX_ERR_NONE with body[0] == '\0' if that script doesn't exist for this package
int alpine_read_trigger_script(const char *control_extract_dir, const char *script_name, char *body, size_t body_len);
int alpine_run_trigger_script(const char *body);

// verifies data_tar_gz_path's compressed bytes against the detached RSA signature in sig_tar_gz_path, using a trusted key from keys_dir - matches Alpine's abuild-sign scheme (sha1/PKCS1v15), used for both .apk control members and APKINDEX.tar.gz
int alpine_verify_signature(const char *data_tar_gz_path, const char *sig_tar_gz_path, const char *keys_dir);

#endif
