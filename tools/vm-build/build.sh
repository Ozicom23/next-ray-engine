#!/usr/bin/env bash
# Builds the working tree on a Windows VM over SSH: a snapshot commit of the tree as it is (uncommitted and untracked
# files included, the real index and branches untouched) is pushed to the VM's repo and built there by build.ps1,
# like the dev build CI. Incremental: only what changed since the last build recompiles.
# Usage: tools/vm-build/build.sh [-c CONF] [--package] [--rebuild] [--fetch] [--install]
#   -c CONF    build configuration (default DX11-AVX)
#   --package  also stage the CI artifact layout (bin, db/mods, mods) on the VM
#   --fetch    copy the result back to _build/vm/ (package/ with --package, else the exes and nvngx_dlss.dll)
#   --install  --fetch, then install into the game: the exes and nvngx_dlss.dll to anomaly/bin (the replaced ones
#              kept in backup-nextray-exes-prev/), gamedata/ to anomaly/gamedata, the MO2 mod to gamma/mods
# Env: NEXTRAY_VM (default windows@192.168.64.4), NEXTRAY_VM_DIR (default C:/nr),
#      NEXTRAY_GAME (default /Volumes/SSD/stalker-gamma+mac.arm64: the folder with anomaly/ and gamma/)

set -euo pipefail

VM=${NEXTRAY_VM:-windows@192.168.64.4}
DIR=${NEXTRAY_VM_DIR:-C:/nr}
GAME=${NEXTRAY_GAME:-/Volumes/SSD/stalker-gamma+mac.arm64}
MOD="neXt-Ray performance patches"
CONF=DX11-AVX
PS_ARGS=()
FETCH=0
PACKAGE=0
INSTALL=0
while [ $# -gt 0 ]; do
    case "$1" in
        -c) CONF=$2; shift ;;
        --package) PS_ARGS+=(-Package); PACKAGE=1 ;;
        --rebuild) PS_ARGS+=(-Rebuild) ;;
        --fetch) FETCH=1 ;;
        --install) FETCH=1; INSTALL=1 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

cd "$(git rev-parse --show-toplevel)"
if [ $INSTALL = 1 ] && [ ! -d "$GAME/anomaly/bin" ]; then
    echo "game folder not found: $GAME/anomaly/bin (set NEXTRAY_GAME)" >&2; exit 1
fi
WIN_DIR=${DIR//\//\\}
vm() { ssh -o LogLevel=ERROR "$VM" "$@"; }

# Snapshot of the working tree through a scratch index
idx=$(mktemp)
trap 'rm -f "$idx"' EXIT
cp "$(git rev-parse --git-path index)" "$idx"
GIT_INDEX_FILE=$idx git add -A
commit=$(git commit-tree "$(GIT_INDEX_FILE=$idx git write-tree)" -p HEAD -m "VM build snapshot")

vm "if not exist $WIN_DIR\\.git (git init -q $WIN_DIR && git -C $WIN_DIR config core.longpaths true && git -C $WIN_DIR config core.autocrlf false)"
# cmd keeps the single quotes git puts around the path, PowerShell strips them
git -c core.sshCommand="ssh -o LogLevel=ERROR" push -q -f --receive-pack="powershell -NoProfile -c git-receive-pack" \
    "$VM:$DIR" "$commit:refs/heads/vm-build"
vm "cd /d $WIN_DIR && git checkout -q -f --detach vm-build && git submodule update -q --init --recursive"

vm "powershell -NoProfile -ExecutionPolicy Bypass -File $WIN_DIR\\tools\\vm-build\\build.ps1 -Conf $CONF ${PS_ARGS[*]:-}"

if [ $FETCH = 1 ]; then
    rm -rf _build/vm && mkdir -p _build/vm
    if [ $PACKAGE = 1 ]; then
        scp -q -o LogLevel=ERROR -r "$VM:$DIR/package" _build/vm/
        bin=_build/vm/package/bin
    else
        scp -q -o LogLevel=ERROR "$VM:$DIR/_build/_game/bin_dbg/*.exe" \
            "$VM:$DIR/src/3rd party/NVIDIA-DLSS/lib/Windows_x86_64/rel/nvngx_dlss.dll" _build/vm/
        bin=_build/vm
    fi
    echo "Fetched to _build/vm/"
fi

if [ $INSTALL = 1 ]; then
    prev="$GAME/backup-nextray-exes-prev"
    rm -rf "$prev" && mkdir -p "$prev"
    for f in "$bin"/*.exe "$bin/nvngx_dlss.dll"; do
        name=$(basename "$f")
        [ -f "$GAME/anomaly/bin/$name" ] && cp -p "$GAME/anomaly/bin/$name" "$prev/"
        cp "$f" "$GAME/anomaly/bin/" && chmod 644 "$GAME/anomaly/bin/$name"
        echo "installed bin/$name"
    done
    # -c: by content, timestamps differ between checkouts; -t so later runs list only real changes
    rsync -rct --itemize-changes gamedata/ "$GAME/anomaly/gamedata/" | sed 's/^/gamedata: /'
    rsync -rct --itemize-changes --delete --exclude meta.ini "mods/$MOD/" "$GAME/gamma/mods/$MOD/" | sed 's/^/mod: /'
    echo "Installed to $GAME (replaced exes in backup-nextray-exes-prev/)"
fi
