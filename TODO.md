# TODO

## Review GPU sysfs binds

The GPU section currently binds these sysfs paths wholesale:

- `/sys/dev/char`
- `/sys/devices`
- `/sys/class/drm`
- `/sys/bus/pci`
- `/sys/module/nvidia` (NVIDIA only)

During debugging of a hang on Surface Pro 7 these were stripped back
progressively, but the actual bug turned out to be unrelated (`set -e`
tripping on bare `[[ ]] && cmd` expressions returning false).

It may be worth revisiting whether the broad `/sys/devices` bind is
needed, or whether binding only the specific GPU device's sysfs subtree
is sufficient for Mesa/Vulkan — and whether any of these paths can block
bwrap on hardware with certain platform drivers.

Ask Claude to help review this when ready.
