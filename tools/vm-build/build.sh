#!/usr/bin/env bash
# Builds the working tree on a Windows VM over SSH: a snapshot commit of the tree as it is (uncommitted and untracked
# files included, the real index and branches untouched) is pushed to the VM's repo and built there by build.ps1,
# like the dev build CI. Incremental: only what changed since the last build recompiles.
# Usage: tools/vm-build/build.sh [-c CONF] [--package] [--rebuild] [--fetch] [--install] [--deck] [--install-to DEST]
#   -c CONF          build configuration (default DX11-AVX)
#   --package        also stage the CI artifact layout (bin, db/mods, mods) on the VM
#   --fetch          copy the result back to _build/vm/ (package/ with --package, else the exes and nvngx_dlss.dll)
#   --install-to DEST  --fetch, then install into the game folder DEST (the one with anomaly/ and gamma/), a local
#                    path or user@host:/path over SSH: the exes and nvngx_dlss.dll to anomaly/bin (the replaced ones
#                    kept in backup-nextray-exes-prev/), gamedata/ to anomaly/gamedata, the MO2 mod to gamma/mods.
#                    Can be given more than once
#   --install        --install-to $NEXTRAY_GAME (default /Volumes/SSD/stalker-gamma+mac.arm64)
#   --deck           --install-to $NEXTRAY_DECK (default deck@192.168.0.230:/home/deck/Games/gamma)
# Env: NEXTRAY_VM (default windows@192.168.64.4), NEXTRAY_VM_DIR (default C:/nr), NEXTRAY_GAME, NEXTRAY_DECK

set -euo pipefail

VM=${NEXTRAY_VM:-windows@192.168.64.4}
DIR=${NEXTRAY_VM_DIR:-C:/nr}
MOD="neXt-Ray performance patches"
CONF=DX11-AVX
PS_ARGS=()
FETCH=0
PACKAGE=0
TARGETS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -c) CONF=$2; shift ;;
        --package) PS_ARGS+=(-Package); PACKAGE=1 ;;
        --rebuild) PS_ARGS+=(-Rebuild) ;;
        --fetch) FETCH=1 ;;
        --install) TARGETS+=("${NEXTRAY_GAME:-/Volumes/SSD/stalker-gamma+mac.arm64}") ;;
        --deck) TARGETS+=("${NEXTRAY_DECK:-deck@192.168.0.230:/home/deck/Games/gamma}") ;;
        --install-to) TARGETS+=("$2"); shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
[ ${#TARGETS[@]} -gt 0 ] && FETCH=1

cd "$(git rev-parse --show-toplevel)"
WIN_DIR=${DIR//\//\\}
vm() { ssh -o LogLevel=ERROR "$VM" "$@"; }

# A target is user@host:/path (SSH) or a local path: on_target runs a shell command in the game folder, at() is
# rsync's name for a path in it
target_host() { case "$1" in *:*) echo "${1%%:*}" ;; esac; }
target_path() { case "$1" in *:*) echo "${1#*:}" ;; *) echo "$1" ;; esac; }
on_target() {
    local host path; host=$(target_host "$1"); path=$(target_path "$1")
    if [ -n "$host" ]; then ssh -o LogLevel=ERROR "$host" "cd '$path' && $2"; else (cd "$path" && bash -c "$2"); fi
}
at() {
    local host; host=$(target_host "$1")
    if [ -n "$host" ]; then echo "$host:'$(target_path "$1")/$2'"; else echo "$1/$2"; fi
}

# Checked before the build: an unmounted drive or a sleeping Deck fails now, not after five minutes
for t in ${TARGETS[@]+"${TARGETS[@]}"}; do
    on_target "$t" "test -d anomaly/bin && test -d gamma/mods" 2>/dev/null ||
        { echo "not a game folder (anomaly/bin, gamma/mods) or not reachable: $t" >&2; exit 1; }
done

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

for t in ${TARGETS[@]+"${TARGETS[@]}"}; do
    echo "--- installing to $t"
    names=$(cd "$bin" && ls *.exe nvngx_dlss.dll | tr '\n' ' ')
    on_target "$t" "rm -rf backup-nextray-exes-prev && mkdir backup-nextray-exes-prev &&
        for f in $names; do if [ -f anomaly/bin/\$f ]; then cp -p anomaly/bin/\$f backup-nextray-exes-prev/; fi; done"
    # -c: by content, timestamps differ between checkouts; -t so later runs list only real changes
    rsync -ct "$bin"/*.exe "$bin/nvngx_dlss.dll" "$(at "$t" anomaly/bin/)"
    on_target "$t" "cd anomaly/bin && chmod 644 $names"
    echo "bin: $names"
    rsync -rct --itemize-changes gamedata/ "$(at "$t" anomaly/gamedata/)" | sed 's/^/gamedata: /'
    rsync -rct --itemize-changes --delete --exclude meta.ini "mods/$MOD/" "$(at "$t" "gamma/mods/$MOD/")" | sed 's/^/mod: /'
    echo "Installed to $t (replaced exes in backup-nextray-exes-prev/)"
done
