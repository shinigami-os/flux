#ifndef FLUX_SHA256_H
#define FLUX_SHA256_H

#include <stddef.h>

// hex-encoded (64 chars + null) sha256 of the file at path, into out (must be >= 65 bytes).
// out[0] is set to '\0' on any failure (missing file, read error) - same contract sha256_of_file
// (util.c's own shell-out wrapper) already had, so every existing caller needs no changes.
void flux_sha256_file(const char *path, char *out, size_t outlen);

// same, but of a plain string (its bytes, not a path) - for the handful of places that hash
// a short input string directly rather than a file's contents
void flux_sha256_string(const char *s, char *out, size_t outlen);

#endif
