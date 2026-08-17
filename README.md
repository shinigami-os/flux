# flux
> Package manager for Kira Linux.

flux is a minimal, source-based package manager written in C. Single binary, no runtime dependencies beyond libc. Every package is built from source against a **kotodama** recipe; binaries are tracked, cached, and removable with full file-level precision.

---

## Design

- **Reproducible builds.** Same recipe + same source = identical output.
- **Binary cache.** Skip compilation when a valid signed binary exists locally or on the remote cache server.
- **Dependency-minimal.** Build deps are only pulled in for a package that actually needs to compile from source. A package with a cache hit, or a meta-package, never drags its build toolchain along.
- **Transparent.** Every operation prints what it is doing and why, with a consistent styled output (bold action headers, indented step lines, bordered tables for install/update queues) that respects `NO_COLOR` and non-tty output automatically.
- **Scriptable.** Exit codes are stable and documented. flux works in shell scripts and CI pipelines.
- **No runtime deps.** flux links only against libc. Nothing else required.
- **Cross-compile aware.** `flux build --cross` builds against a configured cross sysroot instead of the host.

---

## Commands

| Command | Action |
|---|---|
| `flux install <pkg>` | Install a package (from cache or compile from source). If no recipe exists, offers to install a matching app from Flathub instead |
| `flux remove [-a] <pkg>` | Remove a package and all its installed files. `-a`/`--autoremove` also removes now-orphaned auto-installed deps |
| `flux autoremove` | Remove every installed package that's auto-installed and no longer needed by anything |
| `flux update [-i]` | Sync the local recipe repo, report which installed packages have a newer recipe version, and check for a newer flux or kira-base release. `-i` installs the reported updates. |
| `flux search <query>` | Search available recipes by name or description |
| `flux info <pkg>` | Show package details, dependencies, install status |
| `flux list [-a]` | List installed packages, sorted alphabetically. `-a`/`--auto` also includes auto-installed deps |
| `flux build [--cross] <pkg>` | Force local compilation, optionally against the cross sysroot |
| `flux cache clean [--all\|--unused]` | Manage the local binary cache (not yet implemented, stub) |
| `flux compat <pkg>` | Install via Debian compat container (Phase 3) |
| `flux version` | Print the installed flux version |
| `flux self-update` | Rebuild flux from the latest release tag and atomically replace the running binary |
| `flux base-update` | Update kira-base's core image (musl, BusyBox, runit, eudev, curl) to the latest release |
| `flux kernel-update [-f]` | Download, verify, extract, and boot-configure the latest Shinigami kernel release |

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

%post-install
```

flux sets `$DESTDIR` before running `%install`. Recipes install into `$DESTDIR`, flux copies to the live system.

`%post-install` is a fifth, optional hook that runs only during `flux install`, never `flux build`, and operates directly on the real root filesystem instead of `$DESTDIR`. It exists for idempotent system-level mutations that can't be expressed as installed files, like creating a system user. Never write to `$DESTDIR` in this hook, absolute paths here mean the real system.

### Source types

`[source] url` accepts three forms: a direct tarball URL (`sha256` is its real checksum), a bare non-archive file such as a `.ttf` (copied into the build dir under its original name instead of extracted), or `git+<repo>#<ref>` (shallow-cloned; when `<ref>` is a floating branch rather than a tag, `sha256` is repurposed to hold a pinned commit hash instead of a tarball checksum).

### Meta-packages

A recipe with an empty `[source]` is a meta-package: just a dependency list, optionally with a trivial `%install` (drop a few files) or `%post-install` (create a user). Meta-packages never touch the binary cache, local or remote. They re-run their hooks fresh on every build and install. Caching is only for real compiled artifacts from a fetched, checksummed source tree.

Meta-packages are also never gated by the "already installed" check that real packages get. Every `flux install <meta-pkg>` re-walks its full dependency list and re-runs its hooks, even if it was processed before. This is what lets a meta-package pick up new deps or hook changes on a later install without a version bump.

### `no_sysroot_stage`

During `flux build --cross`, every built package's `$DESTDIR` normally also gets copied into `cross_compile_sysroot` so later cross-built packages in the dependency chain can find it via `$FLUX_CROSS_SYSROOT`. A recipe that builds the cross toolchain itself (`gcc`, `binutils`) must not go through this: copying its own output back over the sysroot it was just built with corrupts that toolchain for every future cross build. Set `no_sysroot_stage = true` in `[meta]` to skip the copy for a recipe like that. Omit it (or set anything else) and the default, normal staging behavior applies.

### Hook environment

Every hook gets `DESTDIR` and `FLUX_RECIPE_DIR` (the recipe's own directory, useful for referencing `files/`). During `flux build --cross`, hooks also get `CC`, `CXX`, `AR`, `LD`, `STRIP`, `CPP`, `CROSS_COMPILE`, `FLUX_CROSS_HOST`, and `FLUX_CROSS_SYSROOT`, plus `PKG_CONFIG_PATH`/`PKG_CONFIG_LIBDIR`/`PKG_CONFIG_SYSROOT_DIR` pointed at the cross sysroot.

---

## Configuration

flux reads `/etc/flux/flux.conf` at startup:

```ini
local_repo_path = /var/lib/flux/recipes
remote_repo_url = https://github.com/shinigami-os/flux-recipes
binary_cache_url = https://cache.example.com
default_build_flags = -O2 -pipe -march=x86-64-v2
flux_pub_path = /etc/flux/flux.pub
flux_secret_key_path = /home/user/.minisign/flux.key
cross_compile_prefix = x86_64-linux-musl-
cross_compile_sysroot = /opt/musl-cross/x86_64-linux-musl
cross_toolchain_path = /opt/musl-cross/bin
cross_gcc_libpath = /opt/musl-cross/lib/gcc/x86_64-linux-musl/9.4.0
package_target = x86_64-linux-musl
```

`flux_secret_key_path` only needs to exist on a machine that publishes packages. If it's missing, `flux build`/`flux install` skip cache signing and storage instead of failing.

---

## Package database

Installed packages are tracked in `/var/lib/flux/installed/<pkg>/`:
- `info`: name, version, install date, auto/manual flag
- `files`: one absolute system path per line (empty for meta-packages)

`flux remove` reads the files list and deletes every installed file precisely. No orphaned files.

A package gets `auto_installed = 1` whenever it's pulled in purely as someone else's dependency. If it's later requested directly (`flux install <pkg>` on something already installed), its flag flips to `0` even without reinstalling anything. `flux autoremove` (and `flux remove -a`) only ever touch packages still flagged `1` that nothing else currently depends on.

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

## Versioning

flux uses Kira's own release-based scheme, not semver: `YY.MM`, with an optional `-N` suffix for a hotfix release in that month (`26.06`, then `26.06-1` for the first hotfix). The version is a single compiled-in constant, `FLUX_VERSION` in `include/flux.h` - there's no separate VERSION file to drift out of sync with the binary.

Cutting a release is just `git tag <version> && git push --tags` on the `flux` repo - no GitHub Release object needed. `flux update` and `flux self-update` read tags directly off the remote with `git ls-remote --tags`, pick the highest one with `sort -V`, and compare it against `FLUX_VERSION`. `flux update` just prints a notice if they differ; `flux self-update` does the actual rebuild-and-swap.

## Package update reporting (`flux update` / `flux update -i`)

`flux update` diffs the recipe repo's old and new `HEAD` after syncing (`git diff --name-only <old> <new> -- '*/kotodama'`) to find every recipe that changed. For each changed `<pkg>/kotodama`, if `pkg` is currently installed and its recorded version (`/var/lib/flux/installed/<pkg>/info`) differs from the version now in the recipe, it's reported as `pkg  old -> new`. Recipes that changed but aren't installed, or whose version didn't actually change (a comment tweak, a hook fix without a version bump), are not reported - this is meant to answer "what's outdated on my system," not "what changed upstream."

Plain `flux update` only reports; `flux update -i` additionally force-reinstalls (`flux install -y -f`) every package it just reported, upgrading them to the recipe's current version.

## kira-base updates (`flux base-update`)

`kira-base` (musl, BusyBox, runit, eudev, the bootstrap `dhcpcd`/`curl`) isn't flux-managed and can't rebuild itself from source on an installed system the way flux can - it needs a cross-toolchain and kernel source tree. Instead, `flux update` reads `/etc/kira-release` and checks the `kira-base` repo's tags the same way it checks flux's own; if there's a newer one it just tells you to run `flux base-update`, it never runs automatically.

`flux base-update` fetches that release's `rootfs.tar.gz` and `initramfs.cpio.gz` from `binary_cache_url` (minisign-verified, same key as the package cache), then applies `/etc/kira-update-manifest` from inside the new rootfs - a list `kira-base`'s own Makefile generates, classifying every core file as:
- `live <path>`: safe to atomically replace right now (musl, BusyBox, curl, the CA bundle, the bootstrap `dhcpcd`) - nothing has it loaded as a continuously-running process, so new invocations just pick up the new file.
- `restart:<service> <path>`: replaced now, then that runit service is restarted (`eudev`, since unlike PID 1 it's a normal supervised service).
- `boot <path>`: replaced on disk now, but only takes effect on the next reboot (runit's own `runit-init`/`runsvdir`/`runsv` binaries and the `runit/1,2,3` stage scripts - they're continuously running already, replacing the file doesn't change what's executing in memory).

The new `/boot/initrd.img-<kernel>` is staged the same way, always reboot-required. `flux base-update` tells you at the end whether a reboot is needed.

## Kernel updates (`flux kernel-update`)

The Shinigami kernel is versioned as `<linux-version>-shinigami-<shinigami-version>` (e.g. `7.1.3-shinigami-26.07-4`), read straight off `uname -r`. `flux kernel-update` compares that against `{binary_cache_url}/kira-kernel/latest`, and if there's a newer one: downloads `kira-kernel-<version>.tar.gz` + its `.minisig` from the cache, verifies the signature, extracts it directly onto `/`, runs `depmod -a <version>` to regenerate module dependencies, and updates GRUB if `grub-mkconfig`/`update-grub` is present. The previous kernel's `/lib/modules/<version>` directory is left in place for rollback. Like `base-update`, this never runs automatically (`flux update` just notices a newer version is available and tells you to run it), and it always ends by reporting that a reboot is required.

## Status

Phase 3. `install`, `remove`, `autoremove`, `search`, `update`, `info`, `list`, `build`, `cache`, `version`, `self-update`, `base-update`, `kernel-update` are fully working, including cross-compilation, real per-package dependency resolution, a local + remote binary cache, and a fully-implemented auto-installed/orphan tracking model. `compat` (Debian compat container fallback) is still a stub, not yet implemented.

See the [Kira Linux specification](https://github.com/shinigami-os) and the full project roadmap.

## License

GPL-2.0
