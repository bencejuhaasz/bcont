# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`bcont` is a single bash script that wraps `bwrap` (bubblewrap) to sandbox applications under the Sway compositor. It tags the sandboxed Wayland connection with the `wp_security_context_manager_v1` protocol (via a small C helper) so Sway blocks privileged Wayland protocols (screencopy, virtual-keyboard, layer-shell, data-control, input-method, foreign-toplevel) from sandboxed clients. See README.md for the full feature/flag/profile reference — don't duplicate it here.

## Build / lint / test

```bash
# Build the security-context helper (regenerates protocol bindings, then compiles)
./header_gen.sh   # wayland-scanner: client-header + private-code from security-context-v1.xml
./build.sh        # cc -O2 -Wall -o wayland-security-context-helper ...

# Lint the bash script (the only practical "test" surface here)
shellcheck bcont

# Manually exercise a code path without actually running bwrap
./bcont --dry-run --gpu --ro ~/Videos -- mpv ~/Videos/film.mkv
./bcont firefox --dry-run        # profile-based, prints the bwrap invocation

# Build the Arch package (also runs the build() step above)
makepkg -si       # uses PKGBUILD; pkgver() derives from `git describe`/commit count
```

There is no test suite. Validate changes with `--dry-run` to inspect the assembled `bwrap` argv, with `shellcheck`, and by actually running a profile inside a live Sway session when touching socket/proxy logic.

## Architecture

The whole CLI lives in one file, `bcont` (bash, `set -euo pipefail`). It runs through fixed stages, in this order — most bugs come from getting the ordering or `set -e`-safety of a stage wrong:

1. **Subcommand dispatch** (`list`/`show`/`edit`/`init`/`completion`/`--help`) — short-circuits before any sandbox logic.
2. **Profile resolution** (`load_profile`) — if arg 1 matches a `[section]` in `~/.config/bcont/profiles` (INI-ish, hand-parsed line by line, no external parser), profile values populate `P_*` variables which seed the `ENABLE_*`/`HOME_DIR`/`EXTRA_RO`/`EXTRA_RW` defaults. CLI flags parsed afterward always override profile values.
3. **Flag parsing** via `getopt` into `ENABLE_NET`, `ENABLE_DBUS`, `ENABLE_SYSTEM_DBUS`, `ENABLE_PIPEWIRE`, `ENABLE_PULSE`, `ENABLE_GPU` (0/1/2 = none/full/render), `ENABLE_FIDO`, `FORCE_CLI`, `HOME_DIR`, `EXTRA_RO[]`, `EXTRA_RW[]`, `CMD[]`.
4. **Wayland mode detection** — CLI/headless mode if `--cli` or `$WAYLAND_DISPLAY` unset; otherwise tries the security-context helper, falling back to passing the native socket through unfiltered (with a warning) if Sway is `< 1.9` or the helper binary is missing.
5. **Dependency + Sway version checks** (`bwrap`, `jq` required; `swaymsg` required in Wayland mode; `xdg-dbus-proxy` required if D-Bus is enabled). Sway's security-context support is detected by parsing `swaymsg -t get_version` via `jq`.
6. **Resource setup**, each gated behind its own `ENABLE_*`/`P_*` flag and each appending to its own `*_ARGS` bwrap-argument array (`WAYLAND_SOCKET_ARGS`/`WAYLAND_ENV_ARGS`, `DBUS_ARGS`, `SYSTEM_DBUS_ARGS`, `PIPEWIRE_ARGS`, `PULSE_ARGS`, `GPU_ARGS`, `FIDO_ARGS`, `NET_ARGS`, `HOME_ARGS`, `EXTRA_ARGS`, `ETC_ARGS`):
   - The Wayland security-context socket and any `xdg-dbus-proxy` instances (session + system bus) are spawned as background processes with a poll loop waiting for their socket to appear; all are tracked in PIDs and torn down by `cleanup()` (`trap cleanup EXIT INT TERM`).
   - GPU/FIDO sysfs binds are deliberately narrow (specific subtrees, not the whole `/sys`) — see `TODO.md` for an open question about whether `/sys/devices` is over-broad; comments in `bcont` explain *why* each specific path is needed (NVML, Mesa/Vulkan enumeration, HID device discovery).
   - `EXTRA_RO`/`EXTRA_RW` paths are filtered against a denylist of dangerous targets (`/proc`, `/sys`, `/etc`, `/usr`, `/root`) before being bound.
7. **Argument assembly** into the `BWRAP_ARGS` array — `--tmpfs /etc` plus an explicit curated allowlist of `/etc` files (not a full bind), then `--clearenv` plus an explicit allowlist of env vars. This "default deny, explicit allow" pattern is the core security model and should be preserved when adding new mount/env points.
8. **`exec bwrap "${BWRAP_ARGS[@]}"`** (or, with `--dry-run`, `printf '%q '` the argv instead of executing).

### Conventions specific to this script

- Every stage builds its own `*_ARGS=()` bash array; empty arrays are expanded defensively with `"${ARR[@]+"${ARR[@]}"}"` to survive `set -u` when empty. Follow this pattern for any new optional bwrap argument group.
- Background helper processes (security-context helper, dbus proxies) always: spawn with `&`, capture `$!`, poll for the resulting socket with a bounded retry loop, `die` if the process exits early or the socket never appears, and get killed in `cleanup()`.
- `log_debug`/`log_info`/`log_ok`/`log_warn`/`log_err` go to stderr only; colors are disabled when stderr isn't a TTY.
- New capability flags should follow the existing per-feature pattern end-to-end: a `P_*` profile var + INI key in `load_profile`, a CLI long/short flag in the `getopt` spec, a `ENABLE_*` variable, a dedicated `_ARGS=()` array populated in its own section with `log_debug`/`log_warn` on success/no-op, and a line in `usage()` plus the bash/zsh completion functions in `cmd_completion()`.

### Companion C helper

`wayland-security-context-helper.c` is a separate, small foreground daemon (not part of `bcont` itself): it connects to the compositor, binds `wp_security_context_manager_v1`, creates a new listening Wayland socket, and registers that socket under the given app-id/instance-id as a sandboxed context. It blocks until killed; `bcont` backgrounds it and binds its socket into the sandbox at `wayland-1`. `security-context-v1-client-protocol.h` and `security-context-v1-protocol.c` are generated by `header_gen.sh` from the upstream `security-context-v1.xml` staging protocol — never hand-edit them, regenerate instead.

### Packaging

`PKGBUILD` builds the `bcont-git` AUR package: compiles the C helper, installs `bcont` and the helper binary to `/usr/bin`. Versioning is derived automatically from git (`git describe --tags` if a `vX.Y.Z` tag exists, else `r<commit-count>.<short-hash>`) — don't hand-bump `pkgver` for unrelated changes.
