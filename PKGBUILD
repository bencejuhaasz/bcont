# Maintainer: bencejuhaasz <juhaszbence2001@gmail.com>
pkgname=bcont-git
pkgver=r28.4a40929
pkgrel=1
pkgdesc="Bubblewrap sandbox with Wayland security-context for Sway"
arch=(x86_64 aarch64)
url="https://github.com/bencejuhaasz/bcont"
license=(MIT)
depends=(bubblewrap jq wayland)
makedepends=(git wayland)
optdepends=(
    'sway: Wayland sandbox mode with wp_security_context_manager_v1'
    'xdg-dbus-proxy: D-Bus portal filtering (--dbus flag)'
)
provides=(bcont)
conflicts=(bcont)
source=("$pkgname::git+https://github.com/bencejuhaasz/bcont.git")
sha256sums=('SKIP')

pkgver() {
    cd "$pkgname"
    # Uses tag if available (v1.2.0 → 1.2.0.rN.gHASH), falls back to commit count
    if git describe --long --tags 2>/dev/null | grep -q '^v'; then
        git describe --long --tags | sed 's/^v//;s/\([^-]*-g\)/r\1/;s/-/./g'
    else
        printf "r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
    fi
}

build() {
    cd "$pkgname"
    cc -O2 -Wall \
        -o wayland-security-context-helper \
        wayland-security-context-helper.c \
        security-context-v1-protocol.c \
        -lwayland-client
}

package() {
    cd "$pkgname"
    install -Dm755 bcont                         "$pkgdir/usr/bin/bcont"
    install -Dm755 wayland-security-context-helper "$pkgdir/usr/bin/wayland-security-context-helper"
    install -Dm644 LICENSE                       "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
