#ifndef ALPINE_H
#define ALPINE_H

#include "flux.h"
#include <stddef.h>

#define ALPINE_MAX_NAME_LEN       64
#define ALPINE_MAX_VERSION_LEN    48   // Alpine versions carry -rN suffixes, e.g. "1.2.3-r10"
#define ALPINE_MAX_DESC_LEN       256
#define ALPINE_MAX_URL_LEN        256
#define ALPINE_MAX_LICENSE_LEN    64
#define ALPINE_MAX_ARCH_LEN       16
#define ALPINE_MAX_ORIGIN_LEN     64
#define ALPINE_MAX_CHECKSUM_LEN   96
#define ALPINE_MAX_DEP_TOKEN_LEN  96
// per-call cap when tokenizing one package's D:/p: line at resolve time -
// real edge index has outliers up to ~110 deps / ~255 provides on a few
// split packages, so this is sized well above that, not the common case
#define ALPINE_MAX_TOKENS_PER_LINE 320

typedef struct {
    char token[ALPINE_MAX_DEP_TOKEN_LEN];
} alpine_dep_t;

// one parsed APKINDEX stanza. depends_raw/provides_raw point into the
// owning alpine_index_t's raw_text instead of being copied - across the
// full ~24000-package edge index, storing every D:/p: line as its own
// fixed-size token array would run into the hundreds of MB for outlier
// packages alone. alpine_parse_dep_line() tokenizes on demand, only for
// the one package actually being resolved.
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
int alpine_index_fetch(const flux_config_t *config, const char *repo, const char *arch, char *path_out, size_t path_outlen);
int alpine_index_load(const char *tar_gz_path, alpine_index_t *index);
void alpine_index_free(alpine_index_t *index);
alpine_pkg_t *alpine_index_find(alpine_index_t *index, const char *name);
int alpine_parse_dep_line(const char *raw, alpine_dep_t *out, int max, int *count);

// a .apk is 3 concatenated, independently-valid gzip streams: signature,
// control (.PKGINFO + any pre/post-install scripts), data (the real files).
// downloaded once, then split into 3 separate files so the control member
// can be checked/verified without ever touching the (potentially huge) data
// member, and so only the data member's files land in a package's destdir.
int alpine_apk_url(const flux_config_t *config, const char *repo, const char *arch, const char *name, const char *version, char *out, size_t outlen);
int alpine_apk_download(const flux_config_t *config, const char *repo, const char *arch, const char *name, const char *version, char *path_out, size_t path_outlen);
int alpine_apk_split_members(const char *apk_path, char *sig_path_out, char *control_path_out, char *data_path_out, size_t path_outlen);
int alpine_apk_extract(const char *member_tar_gz_path, const char *destdir);

// main + community held together for the lifetime of one dependency walk,
// so resolving N dependencies doesn't refetch/reparse the index N times
typedef struct {
    alpine_index_t main;
    alpine_index_t community;
} alpine_repos_t;

int alpine_repos_load(const flux_config_t *config, const char *arch, alpine_repos_t *repos);
void alpine_repos_free(alpine_repos_t *repos);
const alpine_pkg_t *alpine_repos_find_by_name(const alpine_repos_t *repos, const char *name, const char **out_repo);
const alpine_pkg_t *alpine_repos_find_provider(const alpine_repos_t *repos, const char *capability_token, const char **out_repo);

#define ALPINE_MAX_RESOLVED_DEPS 128

// resolves one Alpine package's raw D: tokens into concrete package names:
// so:/cmd:/pc: virtual capabilities and plain name+version-constraint tokens
// are all resolved down to whichever real package provides them (greedy,
// first match, no backtracking - see the scope doc). "!pkg" conflict tokens
// are returned separately rather than treated as dependencies to install
int alpine_resolve_deps(const alpine_repos_t *repos, const alpine_pkg_t *pkg,
                         char names_out[][ALPINE_MAX_NAME_LEN], int max_names, int *names_count,
                         char conflicts_out[][ALPINE_MAX_NAME_LEN], int max_conflicts, int *conflicts_count);

#define ALPINE_KEYS_DIR "/etc/flux/alpine-keys"

// verifies the control member's compressed bytes against the detached RSA
// signature carried in the sig member, against a vendored trusted key in
// keys_dir - matches Alpine's own abuild-sign scheme (sha1 digest, RSA
// PKCS1v15, over the control member's raw compressed bytes, not decompressed)
int alpine_verify_control(const char *control_tar_gz_path, const char *sig_tar_gz_path, const char *keys_dir);

#endif
