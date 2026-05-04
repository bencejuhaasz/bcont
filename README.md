# sway-sandbox

Bubblewrap-alapú Wayland-only sandbox Sway compositorhoz, `wp_security_context_manager_v1` támogatással.

## Mit csinál

- `bwrap`-pal minden elérhető namespace-t unshare-el (user, pid, ipc, uts, cgroup, net alapból)
- `/` tmpfs, csak `/usr` és `/etc` read-only bind, minden egyéb whitelist alapján
- Saját `XDG_RUNTIME_DIR` tmpfs-en; csak az engedélyezett socketek bindelve (Wayland + opc. PipeWire + opc. D-Bus proxy)
- Wayland socket security-context-v1 protokollal jelölve — a Sway elutasítja a privileged protokollokat (screencopy, virtual-keyboard, layer-shell, data-control, input-method, foreign-toplevel stb.)
- D-Bus session bus kizárólag `xdg-dbus-proxy`-n keresztül, portálra szűkítve
- XWayland/X11 explicit tiltva: `DISPLAY` és `XAUTHORITY` unset, csak Wayland backend env-ek beállítva
- Minden capability eldobva, új PID namespace-ben PID 1, `--die-with-parent`
- Háló alapból unshare-elve, opcionálisan visszakapcsolható

## Build

```bash
# 1) Security-context helper fordítása (csak egyszer)
cd /hova/raktad
wayland-scanner client-header \
    /usr/share/wayland-protocols/staging/security-context/security-context-v1.xml \
    security-context-v1-client-protocol.h
wayland-scanner private-code \
    /usr/share/wayland-protocols/staging/security-context/security-context-v1.xml \
    security-context-v1-protocol.c
cc -O2 -Wall -o wayland-security-context-helper \
    wayland-security-context-helper.c security-context-v1-protocol.c \
    -lwayland-client
sudo install -m 755 wayland-security-context-helper /usr/local/bin/

# 2) Script telepítése
chmod +x sway-sandbox
sudo install -m 755 sway-sandbox /usr/local/bin/
```

## Függőségek

- `bubblewrap`, `jq`, `swaymsg` (Sway része)
- `xdg-dbus-proxy` (csak ha `--dbus` kell)
- `wayland-protocols` (a staging security-context XML-hez)
- Sway >= 1.9 a security-context támogatáshoz

## Példák

```bash
# Firefox: háló, D-Bus (portálra szűkítve), PipeWire, GPU, perzisztens home
sway-sandbox --name firefox --net --dbus --pipewire --gpu \
    --home ~/sandboxes/firefox -- firefox

# mpv: csak GPU és egy videó olvasásra, semmi háló/dbus
sway-sandbox --name mpv --gpu --ro ~/Videos -- mpv ~/Videos/film.mkv

# Egy gyanús GTK app: minden tiltva, csak a compositorhoz fér hozzá
sway-sandbox --name suspicious -- ./gyanus-binary

# Foot terminál teljesen izoláltan (működik Wayland-natívan)
sway-sandbox --name foot -- foot
```

## Amit érdemes még megnézni

- **Seccomp**: a script jelenleg a bwrap default-jait használja. Komolyabb policy-hoz fordíts BPF-et (pl. Flatpak `flatpak-run.c` bpf-programjából) és add át `--seccomp N`-nel. Külön FD-t kell a bwrap-nek átadni.
- **Portál konfig**: Sway alatt `xdg-desktop-portal-wlr` kell a screencast / fájlválasztó funkciókhoz. A D-Bus proxy ezt már átengedi, de a portál backend külön konfigurálandó.
- **Tűzfal per-sandbox**: ha több sandboxnak külön hálózati policy kell, `--unshare-net` + `slirp4netns` vagy veth pár + nftables. Ez már kívül esik ezen a scripten.
- **GPU privilege**: a `--gpu` a `/dev/dri`-t átadja. Ha több felhasználó / renderernode izoláció kell, érdemes specifikus kártyát (`/dev/dri/renderD128`) bind-elni, és a `card0`-t kihagyni.
