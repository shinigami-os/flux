# flux
> Package manager for Kira Linux.

flux is a minimal, source-based package manager written in C. Single binary, no runtime dependencies beyond libc. Every package is built from source against a **kotodama** recipe; binaries are tracked, cached, and removable with full file-level precision.

---

## Design

- **Reproducible builds.** Same recipe + same source = identical output.
- **Binary cache.** Skip compilation when a valid signed binary exists on the cache server.
- **Dependency-minimal.** Only explicit dependencies are installed. No recommended or suggested auto-installs.
- **Transparent.** Every operation prints exactly what it is doing and why.
- **Scriptable.** Exit codes are stable and documented. flux works in shell scripts and CI pipelines.
- **No runtime deps.** flux links only against libc. Nothing else required.

---

## Commands

| Command | Action |
|---|---|
| `flux install <pkg>` | Install a package (from cache or compile from source) |
| `flux remove <pkg>` | Remove a package and all its installed files |
| `flux update` | Sync the local recipe repo with the remote |
| `flux search <query>` | Search available recipes by name or description |
| `flux info <pkg>` | Show package details, dependencies, install status |
| `flux build <pkg>` | Force local compilation regardless of cache |
| `flux cache <subcommand>` | Manage binary cache |
| `flux compat <pkg>` | Install via Debian compat container (Phase 3) |

---

## kotodama recipe format

Each package is a directory in `flux-recipes/` containing a `kotodama` file. The format uses `[sections]` for declarative metadata and `%hooks` for shell execution blocks.

```
[meta]
name = hello
version = 2.12.1
description = "The classic Hello World program"
license = GPL-3.0
size = 1

[source]
url = https://ftp.gnu.org/gnu/hello/hello-2.12.1.tar.gz
sha256 = 8d99142afd92576f30b0cd7cb42a8dc6809998bc5d607d88761f512e26c7db20

[deps]
build = gcc make
runtime =

[build]
cflags = -O2 -pipe -march=x86-64-v2

%pre-build

%build
./configure --prefix=/usr
make

%post-build

%install
make DESTDIR=$DESTDIR install
```

flux sets `$DESTDIR` before running `%install`. Recipes install into `$DESTDIR`, flux copies to the live system.

---

## Configuration

flux reads `/etc/flux/flux.conf` at startup:

```ini
local_repo_path = /var/lib/flux/recipes
remote_repo_url = https://github.com/shinigami-os/flux-recipes
binary_cache_url = https://github.com/shinigami-os/flux-cache
default_build_flags = -O2 -pipe -march=x86-64-v2
```

---

## Package database

Installed packages are tracked in `/var/lib/flux/installed/<pkg>/`:
- `info`: name, version, install date, auto/manual flag
- `files`: one absolute system path per line

`flux remove` reads the files list and deletes every installed file precisely. No orphaned files.

---

## Building flux

```bash
make
sudo make install   # installs build/flux to /usr/bin/flux
```

Requires: `gcc`, `make`. No other dependencies.

Compiler flags: `-Wall -Wextra -pedantic -std=c11`

---

## Exit codes

| Code | Constant | Meaning |
|------|----------|---------|
| 0 | `FLUX_ERR_NONE` | Success |
| 1 | `FLUX_ERR_GENERAL` | General / unrecoverable error |
| 2 | `FLUX_ERR_USAGE` | Usage error (bad command, missing argument) |
| 3 | `FLUX_ERR_NOT_FOUND` | Package not found |
| 4 | `FLUX_ERR_DEPENDENCY` | Dependency resolution failure |
| 5 | `FLUX_ERR_BUILD` | Build failure |
| 6 | `FLUX_ERR_CACHE` | Cache error |
| 7 | `FLUX_ERR_NETWORK` | Network error |
| 8 | `FLUX_ERR_PERMISSION` | Permission error (needs root) |
| 9 | `FLUX_ERR_CONTAINER` | Compat container error |
| 10 | `FLUX_ERR_SOURCE` | Invalid or unavailable source |
| 11 | `FLUX_ERR_KOTODAMA` | Malformed recipe file |

Exit codes are stable. They will not be renumbered.

---

## Status

Phase 1 complete. `install`, `remove`, `search`, `update`, `info`, `build` are fully working against a local recipe repo. Binary cache and dependency resolution are stubbed; implemented in Phase 2.

See the [Kira Linux specification](https://github.com/shinigami-os) and the full project roadmap.

## License

GPL-2.0
