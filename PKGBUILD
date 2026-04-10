# SPDX-FileCopyrightText: 2026 Alysson Souza e Silva <alysson@ll9.com.br>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Maintainer: Alysson Souza e Silva <alysson@ll9.com.br>

pkgname=plasma-ion-meteireann
pkgver=0.1.1
pkgrel=1
pkgdesc="Met Éireann weather ion for the KDE Plasma weather widget"
arch=('x86_64')
license=('GPL-2.0-or-later' 'LGPL-2.0-or-later')
install=plasma-ion-meteireann.install
depends=(
  'gcc-libs'
  'kcoreaddons'
  'kdeplasma-addons'
  'ki18n'
  'kio'
  'kunitconversion'
  'qt6-base'
  'qt6-declarative'
)
makedepends=(
  'cmake'
  'extra-cmake-modules'
)

build() {
  cmake -B "${srcdir}/build" -S "${startdir}" \
    -DCMAKE_BUILD_TYPE=None \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DKDE_INSTALL_LIBDIR=lib
  cmake --build "${srcdir}/build"
}

package() {
  DESTDIR="${pkgdir}" cmake --install "${srcdir}/build"
}
