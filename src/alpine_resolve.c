#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/alpine.h"
#include "../include/util.h"

int alpine_repos_load(const flux_config_t *config, const char *arch, alpine_repos_t *repos) {
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

static const alpine_pkg_t *find_provider_in(const alpine_index_t *idx, const char *bare_token, const char **out_repo, const char *repo_name) {
    for (int i = 0; i < idx->count; i++) {
        if (provides_matches(idx->pkgs[i].provides_raw, bare_token)) {
            if (out_repo) *out_repo = repo_name;
            return &idx->pkgs[i];
        }
    }
    return NULL;
}

const alpine_pkg_t *alpine_repos_find_provider(const alpine_repos_t *repos, const char *capability_token, const char **out_repo) {
    char bare[ALPINE_MAX_DEP_TOKEN_LEN];
    strip_constraint(capability_token, bare, sizeof(bare));

    const alpine_pkg_t *p = find_provider_in(&repos->main, bare, out_repo, "main");
    if (p) return p;
    return find_provider_in(&repos->community, bare, out_repo, "community");
}

int alpine_resolve_deps(const alpine_repos_t *repos, const alpine_pkg_t *pkg,
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
        if (strncmp(bare, "so:", 3) == 0 || strncmp(bare, "cmd:", 4) == 0 || strncmp(bare, "pc:", 3) == 0) {
            provider = alpine_repos_find_provider(repos, bare, NULL);
        } else {
            provider = alpine_repos_find_by_name(repos, bare, NULL);
            if (!provider) provider = alpine_repos_find_provider(repos, bare, NULL);
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
