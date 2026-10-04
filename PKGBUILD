# Maintainer: Adrian <adrian@mxlinux.org>
pkgname=mx-packageinstaller
pkgver=${PKGVER:-26.10}
pkgrel=1
pkgdesc="MX Package Installer - a tool for managing packages and Flatpaks"
arch=('x86_64' 'i686')
url="https://mxlinux.org"
license=('GPL3')
depends=('qt6-base' 'polkit' 'flatpak')
makedepends=('cmake' 'ninja' 'qt6-tools')
optdepends=('paru: AUR helper for AUR tab operations and Snap setup (snapd is built from the AUR)')
source=()
sha256sums=()

build() {
    cd "${startdir}"
    cmake -G Ninja -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DPACKAGE_BACKEND=pacman
    cmake --build build --parallel
}

check() {
    cd "${startdir}"
    cmake -B build -DBUILD_TESTS=ON
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure
}

package() {
    cd "${startdir}"
    install -Dm755 build/mx-packageinstaller "${pkgdir}/usr/bin/mx-packageinstaller"
    install -dm755 "${pkgdir}/usr/share/mx-packageinstaller/locale"
    install -Dm644 build/*.qm "${pkgdir}/usr/share/mx-packageinstaller/locale/" 2>/dev/null || true
    install -dm755 "${pkgdir}/usr/lib/mx-packageinstaller"
    install -Dm755 build/helper "${pkgdir}/usr/lib/mx-packageinstaller/helper"
    install -Dm755 scripts/mxpi-lib "${pkgdir}/usr/lib/mx-packageinstaller/mxpi-lib"
    install -Dm755 scripts/mxpi-maintenance-pacman "${pkgdir}/usr/lib/mx-packageinstaller/mxpi-maintenance"
    install -Dm644 build/org.mxlinux.pkexec.mxpi-helper.policy \
        "${pkgdir}/usr/share/polkit-1/actions/org.mxlinux.pkexec.mxpi-helper.policy"
    install -Dm644 mx-packageinstaller.desktop "${pkgdir}/usr/share/applications/mx-packageinstaller.desktop"
    install -Dm644 icons/mx-packageinstaller.png "${pkgdir}/usr/share/icons/hicolor/48x48/apps/mx-packageinstaller.png"
    install -Dm644 icons/mx-packageinstaller.png "${pkgdir}/usr/share/pixmaps/mx-packageinstaller.png"
    install -Dm644 icons/mx-packageinstaller.svg "${pkgdir}/usr/share/icons/hicolor/scalable/apps/mx-packageinstaller.svg"
    install -Dm644 debian/mx-packageinstaller.1 "${pkgdir}/usr/share/man/man1/mx-packageinstaller.1"
    install -dm755 "${pkgdir}/usr/share/doc/mx-packageinstaller"
    install -Dm644 help/mx-package-installer-pacman.html \
        "${pkgdir}/usr/share/doc/mx-packageinstaller/mx-package-installer-pacman.html"
    install -Dm644 help/license.html "${pkgdir}/usr/share/doc/mx-packageinstaller/license.html"
    for shot in pacman-1.png pacman-2.png pacman-3.png pacman-4.png; do
        if [ -f "help/${shot}" ]; then
            install -Dm644 "help/${shot}" "${pkgdir}/usr/share/doc/mx-packageinstaller/${shot}"
        fi
    done
}
