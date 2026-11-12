# Sourced, not run, by the scripts in tools/. Needs $REPO, the root of this repository.
#
# Settings come from the environment or from tools/local.conf, a shell file that is not tracked
# (copy tools/local.conf.example). The environment wins over local.conf.
#
#   WINE_DEV_TREE      the dev tree: a git tree of Wine whose commits after tag pristine-<ver>
#                      are this repo's patches, one per commit. tools/devtree.sh makes one.
#   WINE_BUILD_HOST    ssh host of the RenkuOS build machine (an ~/.ssh/config alias is easiest)
#   WINE_BUILD_PORTS   the haikuports checkout on that machine, relative to its home directory
#                      (default: haikuports)
#   WINE_PORT_VERSION  act on another Wine version's port than the newest recipe here
#   WINE_UPSTREAM_TREE, RENKU_SOURCE_TREE   optional local clones, shown by tools/status.sh
#
# Derived:
#   VER       port version, from the newest wine-<ver>.recipe in app-emulation/wine/
#   PRISTINE  the dev tree's tag on that version's pristine import, pristine-<ver>
#   RECIPE / PSET   repo-relative paths of the recipe and the patchset
#   DEV, HOST, PORTS   the settings above

_cs_dev=${WINE_DEV_TREE-} _cs_host=${WINE_BUILD_HOST-} _cs_ports=${WINE_BUILD_PORTS-}
_cs_ver=${WINE_PORT_VERSION-}
[ -f "$REPO/tools/local.conf" ] && . "$REPO/tools/local.conf"
[ -n "$_cs_dev" ] && WINE_DEV_TREE=$_cs_dev
[ -n "$_cs_host" ] && WINE_BUILD_HOST=$_cs_host
[ -n "$_cs_ports" ] && WINE_BUILD_PORTS=$_cs_ports
[ -n "$_cs_ver" ] && WINE_PORT_VERSION=$_cs_ver
unset _cs_dev _cs_host _cs_ports _cs_ver

_cs_pv=$(ls "$REPO"/app-emulation/wine/wine-*.recipe 2>/dev/null \
         | sed -n 's|.*/wine-\([0-9][0-9.]*\)\.recipe$|\1|p' | sort -V | tail -1)
VER="${WINE_PORT_VERSION:-$_cs_pv}"
unset _cs_pv
PRISTINE="pristine-$VER"
RECIPE="app-emulation/wine/wine-$VER.recipe"
PSET="app-emulation/wine/patches/wine-$VER.patchset"

DEV="${WINE_DEV_TREE:-}"
HOST="${WINE_BUILD_HOST:-}"
PORTS="${WINE_BUILD_PORTS:-haikuports}"

need_dev() {
    [ -n "$DEV" ] || { echo "WINE_DEV_TREE is not set (see tools/local.conf.example)" >&2; exit 2; }
    [ -d "$DEV/.git" ] || { echo "$DEV is not a git tree; make one with tools/devtree.sh" >&2; exit 2; }
}
need_host() {
    [ -n "$HOST" ] || { echo "WINE_BUILD_HOST is not set (see tools/local.conf.example)" >&2; exit 2; }
}
