# bcont

A bubblewrap-based sandbox for Sway that uses the `wp_security_context_manager_v1` Wayland protocol to mark sandboxed connections, causing Sway to block privileged Wayland protocols from them.

## What it does

- Runs applications in Linux namespaces (user, pid, ipc, uts, cgroup, net) via `bwrap`
- Mounts a minimal filesystem: tmpfs root, `/usr` and a curated `/etc` subset read-only, everything else whitelisted explicitly
- Creates a private `XDG_RUNTIME_DIR` on a tmpfs; only permitted sockets are exposed (Wayland, optional PipeWire, optional D-Bus proxy)
- Tags the Wayland socket with `wp_security_context_manager_v1` — Sway then refuses privileged protocols on that connection (screencopy, virtual-keyboard, layer-shell, data-control, input-method, foreign-toplevel, etc.)
- Filters D-Bus through `xdg-dbus-proxy`, scoped to portal interfaces only
- Explicitly blocks X11: `DISPLAY` and `XAUTHORITY` are unset; only Wayland backend env vars are set
- Drops all capabilities, runs as PID 1 in a new PID namespace, `--die-with-parent`
- Network blocked by default, re-enabled with `--net`
- Named profiles in `~/.config/bcont/profiles` for per-app configuration

## Installation

### AUR (recommended for Arch Linux)

```bash
# Using an AUR helper
paru -S bcont-git
# or
yay -S bcont-git
```

### Manual

```bash
# Build the security-context helper
wayland-scanner client-header \
    /usr/share/wayland-protocols/staging/security-context/security-context-v1.xml \
    security-context-v1-client-protocol.h
wayland-scanner private-code \
    /usr/share/wayland-protocols/staging/security-context/security-context-v1.xml \
    security-context-v1-protocol.c
cc -O2 -Wall -o wayland-security-context-helper \
    wayland-security-context-helper.c security-context-v1-protocol.c \
    -lwayland-client

# Install
sudo install -m 755 wayland-security-context-helper /usr/local/bin/
sudo install -m 755 bcont /usr/local/bin/
```

## Dependencies

| Package | Required | Purpose |
|---|---|---|
| `bubblewrap` | yes | namespace sandboxing |
| `jq` | yes | parse Sway version |
| `sway` (swaymsg) | yes | Wayland compositor + security-context support |
| `xdg-dbus-proxy` | for `--dbus` | D-Bus portal filtering |
| `wayland` / `wayland-protocols` | build only | compile the security-context helper |

Sway 1.9 or newer is required for `wp_security_context_manager_v1`. On older Sway the native socket is passed through with a warning.

## Quick start

```bash
# Create the starter config with example profiles
bcont init

# List your profiles
bcont list

# Run a profile
bcont firefox

# Drop into a shell inside a sandbox to explore it
bcont firefox --shell

# Run a profile with an extra flag appended to its command
bcont firefox -- --private-window

# Override a profile setting on the fly (-N enables network even if the profile has net=false)
bcont firefox -N

# Direct use without a profile
bcont --gpu --ro ~/Videos -- mpv ~/Videos/film.mkv
```

## Profile config

Profiles live in `~/.config/bcont/profiles` (INI format). Create the file with `bcont init`.

```ini
[firefox]
net       = true
dbus      = true
pipewire  = true
gpu       = full
home      = ~/.local/share/bcont/firefox
cmd       = firefox

[mpv]
gpu       = full
ro        = ~/Videos
cmd       = mpv

[foot]
cmd       = foot

[zathura]
gpu       = render
ro        = ~/Documents
cmd       = zathura
```

### Profile keys

| Key | Values | Description |
|---|---|---|
| `name` | string | Override sandbox identity / app-id (default: section name) |
| `net` | true / false | Allow network access |
| `dbus` | true / false | Filter D-Bus through xdg-dbus-proxy |
| `pipewire` | true / false | Expose the PipeWire socket |
| `gpu` | full / render / false | GPU access: `full` includes KMS/DRM, `render` is render nodes only |
| `cli` | true / false | Headless mode — no Wayland socket |
| `home` | path | Persistent home directory (tmpfs if omitted) |
| `ro` | path | Read-only bind mount (repeatable) |
| `rw` | path | Read-write bind mount (repeatable) |
| `cmd` | command | Default command to run |

## CLI reference

```
bcont [OPTIONS] -- <command> [args...]
bcont <profile>  [OPTIONS] [-- extra-args]
bcont list | show <profile> | edit | init | completion bash|zsh
```

### Flags

| Flag | Description |
|---|---|
| `-s`, `--shell` | Run `$SHELL` inside the sandbox instead of the configured command |
| `-N`, `--net` | Enable network access |
| `-d`, `--dbus` | Enable D-Bus via portal proxy |
| `-p`, `--pipewire` | Expose the PipeWire audio socket |
| `-g`, `--gpu` | Full GPU access (`/dev/dri` + `/dev/nvidia*`) |
| `--gpu-render` | Render nodes only (no KMS/DRM card devices) |
| `--cli` | Force headless mode, no Wayland socket |
| `-H`, `--home DIR` | Persistent home directory |
| `--ro PATH` | Read-only bind (repeatable) |
| `--rw PATH` | Read-write bind (repeatable) |
| `-n`, `--name NAME` | Sandbox identity / Wayland app-id |
| `--dry-run` | Print the bwrap command without running it |
| `-v`, `--verbose` | Show debug output |

### Subcommands

| Subcommand | Description |
|---|---|
| `list`, `ls` | List all saved profiles |
| `show <profile>` | Print a profile's settings |
| `edit` | Open the config file in `$EDITOR` |
| `init` | Create a starter config with example profiles |
| `completion bash\|zsh` | Print a shell completion script |

### Shell completion

```bash
# Bash — add to ~/.bashrc
source <(bcont completion bash)

# Zsh — save to a directory on $fpath
bcont completion zsh > ~/.zfunc/_bcont
```

## GPU support (NVIDIA)

The `--gpu` flag binds the following into the sandbox:

- `/dev/dri/` — DRM/KMS and render nodes
- `/dev/nvidia*` — NVIDIA character devices
- `/dev/nvidia-caps/` — NVIDIA capability devices
- `/sys/dev/char`, `/sys/devices`, `/sys/class/drm`, `/sys/bus/pci` — device topology
- `/sys/module/nvidia` — required by NVML for GPU enumeration

All sysfs paths are read-only. NVIDIA device files require rw because the kernel driver uses write-side ioctls even for read operations.

## Security notes

**What is blocked with the Wayland security context:**
- `wlr-screencopy-unstable-v1` (screen capture)
- `virtual-keyboard-unstable-v1` (keyboard injection)
- `wlr-layer-shell-unstable-v1` (overlay windows)
- `wlr-data-control-unstable-v1` (clipboard access)
- `input-method-unstable-v1`
- `wlr-foreign-toplevel-management-unstable-v1`

**What is not covered by this sandbox:**
- Seccomp syscall filtering: bcont uses bwrap's defaults. For stricter policy, compile a BPF filter and pass it with `--seccomp`.
- Per-sandbox networking: `--net` shares the host network stack. For isolated network namespaces, combine `--unshare-net` (the default) with `slirp4netns` or a veth pair.
- Portal backends: file picker and screenshare portals require `xdg-desktop-portal-wlr` (or equivalent) configured separately.

## License

MIT — see [LICENSE](LICENSE).
