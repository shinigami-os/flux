#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../include/alpine.h"
#include "../include/util.h"

int alpine_repos_sync(const flux_config_t *config, const char *arch, alpine_repos_t *repos) {
    memset(repos, 0, sizeof(*repos));

    const char *names[2] = { "main", "community" };
    alpine_index_t *slots[2] = { &repos->main, &repos->community };

    for (int i = 0; i < 2; i++) {
        char idx_path[512];
        if (alpine_index_fetch(config, names[i], arch, idx_path, sizeof(idx_path)) != FLUX_ERR_NONE) continue;
        alpine_index_load(idx_path, slots[i]);
    }

    if (repos->main.count == 0 && repos->community.count == 0) return FLUX_ERR_NETWORK;
    return FLUX_ERR_NONE;
}

int alpine_repos_load(const flux_config_t *config, const char *arch, alpine_repos_t *repos) {
    (void)config;
    memset(repos, 0, sizeof(*repos));

    const char *names[2] = { "main", "community" };
    alpine_index_t *slots[2] = { &repos->main, &repos->community };

    for (int i = 0; i < 2; i++) {
        char idx_path[512];
        alpine_index_cache_path(names[i], arch, idx_path, sizeof(idx_path));
        struct stat st;
        if (stat(idx_path, &st) != 0) continue;
        alpine_index_load(idx_path, slots[i]);
    }

    if (repos->main.count == 0 && repos->community.count == 0) return FLUX_ERR_CACHE;
    return FLUX_ERR_NONE;
}

void alpine_repos_free(alpine_repos_t *repos) {
    alpine_index_free(&repos->main);
    alpine_index_free(&repos->community);
}

const alpine_pkg_t *alpine_repos_find_by_name(const alpine_repos_t *repos, const char *name, const char **out_repo) {
    alpine_pkg_t *p = alpine_index_find((alpine_index_t *)&repos->main, name);
    if (p) { if (out_repo) *out_repo = "main"; return p; }
    p = alpine_index_find((alpine_index_t *)&repos->community, name);
    if (p) { if (out_repo) *out_repo = "community"; return p; }
    return NULL;
}

// strips a trailing version constraint, e.g. "musl>=1.2.5-r0" -> "musl" - the resolver is greedy/unversioned
static void strip_constraint(const char *token, char *out, size_t outlen) {
    size_t i = 0;
    while (token[i] && token[i] != '=' && token[i] != '<' && token[i] != '>' && token[i] != '~' && i < outlen - 1) {
        out[i] = token[i];
        i++;
    }
    out[i] = '\0';
}

static int provides_matches(const char *provides_raw, const char *bare_token) {
    if (!provides_raw) return 0;

    alpine_dep_t tokens[ALPINE_MAX_TOKENS_PER_LINE];
    int count = 0;
    alpine_parse_dep_line(provides_raw, tokens, ALPINE_MAX_TOKENS_PER_LINE, &count);

    for (int i = 0; i < count; i++) {
        char bare[ALPINE_MAX_DEP_TOKEN_LEN];
        strip_constraint(tokens[i].token, bare, sizeof(bare));
        if (strcmp(bare, bare_token) == 0) return 1;
    }
    return 0;
}

// a capability can have more than one provider (e.g. polkit-elogind-libs vs polkit-noelogind-libs
// both provide "polkit-libs") - prefer whichever one is already installed on disk, since that's a
// choice a *previous*, separate `flux install` invocation already committed to; without this, two
// installs of the same system can each pick a different alternative and conflict on shared files.
// Only falls back to the first index match (old behavior) when none of them are installed yet.
static const alpine_pkg_t *find_provider_in(const alpine_index_t *idx, const char *bare_token, const char **out_repo, const char *repo_name) {
    const alpine_pkg_t *first = NULL;
    for (int i = 0; i < idx->count; i++) {
        if (!provides_matches(idx->pkgs[i].provides_raw, bare_token)) continue;
        if (!first) first = &idx->pkgs[i];
        if (flux_db_is_installed(idx->pkgs[i].name)) {
            if (out_repo) *out_repo = repo_name;
            return &idx->pkgs[i];
        }
    }
    if (first && out_repo) *out_repo = repo_name;
    return first;
}

const alpine_pkg_t *alpine_repos_find_provider(const alpine_repos_t *repos, const char *capability_token, const char **out_repo) {
    char bare[ALPINE_MAX_DEP_TOKEN_LEN];
    strip_constraint(capability_token, bare, sizeof(bare));

    const alpine_pkg_t *main_match = find_provider_in(&repos->main, bare, out_repo, "main");
    if (main_match && flux_db_is_installed(main_match->name)) return main_match;

    const alpine_pkg_t *community_match = find_provider_in(&repos->community, bare, out_repo, "community");
    if (community_match && flux_db_is_installed(community_match->name)) return community_match;

    if (main_match) { if (out_repo) *out_repo = "main"; return main_match; }
    return community_match;
}

int alpine_resolve_deps(const alpine_repos_t *repos, const alpine_pkg_t *pkg,
                         const char already_selected[][ALPINE_MAX_NAME_LEN], int already_selected_count,
                         char names_out[][ALPINE_MAX_NAME_LEN], int max_names, int *names_count,
                         char conflicts_out[][ALPINE_MAX_NAME_LEN], int max_conflicts, int *conflicts_count) {
    *names_count = 0;
    *conflicts_count = 0;

    alpine_dep_t tokens[ALPINE_MAX_TOKENS_PER_LINE];
    int token_count = 0;
    alpine_parse_dep_line(pkg->depends_raw, tokens, ALPINE_MAX_TOKENS_PER_LINE, &token_count);

    for (int i = 0; i < token_count; i++) {
        const char *tok = tokens[i].token;

        if (tok[0] == '!') {
            if (*conflicts_count < max_conflicts) {
                char bare[ALPINE_MAX_DEP_TOKEN_LEN];
                strip_constraint(tok + 1, bare, sizeof(bare));
                strncpy(conflicts_out[*conflicts_count], bare, ALPINE_MAX_NAME_LEN - 1);
                (*conflicts_count)++;
            }
            continue;
        }

        char bare[ALPINE_MAX_DEP_TOKEN_LEN];
        strip_constraint(tok, bare, sizeof(bare));
        if (bare[0] == '\0') continue;

        const alpine_pkg_t *provider = NULL;
        // an exact package name always wins outright, whether or not it's also a capability
        // token (so:/cmd:/pc: are never real package names, so skip this for those)
        if (strncmp(bare, "so:", 3) != 0 && strncmp(bare, "cmd:", 4) != 0 && strncmp(bare, "pc:", 3) != 0)
            provider = alpine_repos_find_by_name(repos, bare, NULL);

        if (!provider) {
            // this is a capability/virtual name with more than one possible provider (e.g. bare
            // "polkit-libs", or "so:libpolkit-gobject-1.so.0") - a package can list both an exact
            // alternative (e.g. "polkit-noelogind-libs") and the generic capability it already
            // provides in the same D: line, or a *different* package can independently depend on
            // the same virtual name - either way, if something already selected a provider for it,
            // reuse that one instead of picking a different (and possibly conflicting) alternative
            int already_satisfied = 0;
            for (int j = 0; j < *names_count && !already_satisfied; j++) {
                const alpine_pkg_t *existing = alpine_repos_find_by_name(repos, names_out[j], NULL);
                if (existing && provides_matches(existing->provides_raw, bare)) already_satisfied = 1;
            }
            // also check every package already committed elsewhere in this install's dependency graph -
            // a sibling dependency processed before this one can already have claimed the same capability
            for (int j = 0; j < already_selected_count && !already_satisfied; j++) {
                const alpine_pkg_t *existing = alpine_repos_find_by_name(repos, already_selected[j], NULL);
                if (existing && provides_matches(existing->provides_raw, bare)) already_satisfied = 1;
            }
            if (already_satisfied) continue;

            provider = alpine_repos_find_provider(repos, bare, NULL);
        }

        if (!provider) {
            flux_warn("could not resolve Alpine dependency '%s' of '%s', skipping", bare, pkg->name);
            continue;
        }

        int dup = 0;
        for (int j = 0; j < *names_count; j++)
            if (strcmp(names_out[j], provider->name) == 0) { dup = 1; break; }
        if (dup) continue;

        if (*names_count < max_names) {
            strncpy(names_out[*names_count], provider->name, ALPINE_MAX_NAME_LEN - 1);
            (*names_count)++;
        }
    }
    return FLUX_ERR_NONE;
}
