pkgname=meld-qt
pkgver=r2.181f146
pkgrel=1
pkgdesc="Visual diff and merge tool"
url="https://github.com/Alexander-r/meld-qt"
license=('MIT')
depends=('qt6-base' 'hicolor-icon-theme')
makedepends=('qt6-tools' 'cmake')
arch=('x86_64')
source=('git+https://github.com/Alexander-r/meld-qt.git')
md5sums=('SKIP')

pkgver() {
  cd "$pkgname"
  printf "r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

build() {
  cd "$pkgname"
  cmake -B build \
    -DMELD_SINGLE_INSTANCE_NETWORK=ON \
    -DMELD_BUILD_TESTS=OFF \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_BUILD_TYPE=None
  cmake --build build
}

package() {
  cd "$pkgname"
  DESTDIR="$pkgdir" cmake --install build
  install -Dm644 LICENSE -t "$pkgdir/usr/share/licenses/$pkgname/"
}
