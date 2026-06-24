# Xwayland segfault on NVIDIA 580.159.04 (not a bcont bug)

## Symptom

Running `Xwayland` (e.g. manually inside the `steam` bcont sandbox, or via
Steam itself) crashes shortly after a client connects:

```
(EE) Backtrace:
(EE) 0: Xwayland (...+0x18f00a) [...]
(EE) 1: Xwayland (...+0x18f115) [...]
(EE) 2: /usr/lib/libc.so.6 (...+0x3e8f0) [...]
(EE) 3: Xwayland (...+0x3daff) [...]
(EE) 4: Xwayland (...+0x2ee8a) [...]
(EE) 5: Xwayland (...+0x197da2) [...]
(EE) 6: Xwayland (...+0xa7cbf) [...]
(EE) 7: Xwayland (...+0x1d9fa) [...]
(EE) 8: /usr/lib/libc.so.6 (__libc_start_main+0x89) [...]
(EE) 9: Xwayland (...+0x1f3f5) [...]
(EE)
(EE) Segmentation fault at address 0x0
Fatal server error:
(EE) Caught signal 11 (Segmentation fault). Server aborting
```

## Root cause

Confirmed via `gdb` + Arch's debuginfod (build-id
`05f7f6a6a39760cce448b17c9bdb1609105d24a2`, package `xorg-xwayland 24.1.12-1`):

```
#0 xorg_backtrace                                   (crash-handler frame, not real)
#1 OsSigHandler            os/osinit.c:138           (crash-handler frame, not real)
#2 dixGetPrivate           include/privates.h:137    <- NULL pixmap private deref
#3 xwl_window_buffer_destroy_pixmap
                           hw/xwayland/xwayland-window-buffers.c:100
#4 ospoll_wait             os/ospoll.c:644
#5 WaitForSomething        os/WaitFor.c:206
#6 dix_main                dix/main.c:279
#7 _start
```

This is a NULL-pointer dereference inside Xwayland's glamor/DRI3 buffer
teardown path (`xwl_window_buffer_destroy_pixmap`), running against the
NVIDIA proprietary driver (`580.159.04`). It is a use-after-free/race in
Xwayland itself, **not** anything related to bcont's sandbox/bwrap setup.

## Reproduction (no Steam, no bcont sandbox needed)

Confirmed to reproduce with bare `Xwayland` + GPU passthrough, triggered
by a second client connecting/creating a window right after another
client's connection cycle:

```sh
Xwayland &
export DISPLAY=:0
glxinfo            # first client connects/disconnects cleanly
xeyes &            # second client connect -> SIGSEGV in Xwayland
```

The backtrace offsets from this repro match the bottom 7 frames of the
original crash log exactly (same binary, same bug).

## Workaround (recommended): `-noreset`

In both the original crash log and every repro, the crash happens
**immediately after a server reset** — i.e. the first client to connect
right after the *previous* client disconnected and Xwayland ran its
automatic reset cycle (the `"1 XSELINUXs still allocated at reset"` /
`PIXMAP`/`GC` accounting block that appears right before the crash).
Xwayland's `-noreset` flag skips that reset-on-last-client-disconnect
cycle entirely.

```sh
Xwayland -noreset
```

Verified: with `-noreset`, 5 full cycles of `glxgears` (real GPU
rendering) + `xeyes` connecting and disconnecting ran with **glamor
still on** — `direct rendering: Yes`, `OpenGL renderer string: Quadro
P3000/PCIe/SSE2`, `OpenGL vendor string: NVIDIA Corporation` — and no
crash. The same connect/disconnect pattern that reliably crashed
Xwayland in the default config survived all 5 cycles with `-noreset`.

This avoids the bug at its actual trigger condition (the reset path)
instead of disabling glamor wholesale, so there's no rendering-quality
tradeoff: full NVIDIA hardware acceleration stays on and the tearing
described below never comes into play.

## Workaround (fallback, has a downside): `-glamor off`

If `-noreset` ever turns out insufficient for some other trigger of
this bug, disabling glamor avoids the buggy buffer-teardown path
entirely, at a cost — see below.

```sh
Xwayland -glamor off
# or
XWAYLAND_NO_GLAMOR=1 Xwayland
```

Verified: with `-glamor off`, the same glxinfo -> xeyes -> second client
sequence runs with no crash.

**Tradeoff:** legacy GLX/OpenGL clients inside that Xwayland instance
render in software (Mesa llvmpipe) instead of via the NVIDIA driver.
Vulkan-based rendering (DXVK/VKD3D, i.e. most modern Proton games) is
unaffected, since Vulkan talks to `/dev/dri` directly and bypasses
Xwayland's renderer entirely.

## Known side effect of the workaround: tearing

With `-glamor off`, Xwayland presents via `wl_shm` buffers instead of
glamor's synced DRI3/Present path, so screen updates aren't reliably
gated on the compositor's frame callback. This shows up as a
rolling-shutter-like tearing/wobble during fast content updates (e.g.
quickly jiggling the mouse over a window).

Confirmed **not** bcont-specific: the same tearing reproduces running
`Xwayland -glamor off` directly on the host Sway session, with no bwrap
sandbox involved at all. It's an inherent tradeoff of the workaround
itself (software/shm presentation has no synced frame-callback gating),
not something introduced by bcont's `wp_security_context_manager_v1`
socket handling.

Both Xwayland 24.1.12 and Sway 1.12 (wlroots) already support explicit
sync (`wp_linux_drm_syncobj_v1` / `wlr_linux_drm_syncobj_manager_v1`),
which is the modern mechanism meant to fix glamor/DRI3 buffer races like
the crash above without needing software fallback — but it only applies
to the glamor/DRI3 path, not `-glamor off`'s shm path. So re-enabling
glamor (once the upstream crash is fixed) should resolve both the crash
*and* the tearing; `-glamor off` only trades one problem for the other.

## Real fix

Needs an upstream fix from either Xwayland (glamor buffer-teardown race)
or the NVIDIA driver. Check for newer `xorg-xwayland` / `nvidia` package
versions before relying on the `-glamor off` workaround long-term — once
glamor can be re-enabled without crashing, the tearing goes away too.

## Related: gamescope-nvidia, mouse clicks not registering in GamepadUI

Switching from bare Xwayland to `gamescope-nvidia` (AUR, NVIDIA-patched
fork at `sharkautarch/gamescope`, branch `nvidia-fix`) avoids the crash
above entirely and is the preferred way to run Steam in this sandbox.

One issue found: mouse clicks didn't register when Steam was launched
in `-gamepadui` (Big Picture) mode inside gamescope-nvidia. Launching
normal (desktop-mode) Steam inside gamescope-nvidia works fine — clicks
work as expected. Root cause wasn't traced further (not a
glamor/protocol issue we dug into; user confirmed by testing both
modes). If this resurfaces, start from "is it gamepadui-mode specific"
rather than re-diagnosing the whole input path from scratch.
