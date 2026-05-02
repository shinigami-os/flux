# flux
> Package manager for Kira Linux.

flux is a minimal, source-based package manager written in C. Single binary, no runtime dependencies beyond libc. Every package is built from source against a recipe; binaries are signed and cached on a self-hosted server.

## Key properties
- Reproducible builds: same recipe + source = identical output
- Binary cache: skip compilation when a valid signed binary exists
- Dependency-minimal: only explicit dependencies are installed
- Transparent: every operation is logged and explained

## Status
Pre-development. See the [Kira Linux specification](https://github.com/shinigami-os) and the project roadmap.

## License
GPL-2.0
