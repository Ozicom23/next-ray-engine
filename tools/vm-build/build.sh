#!/usr/bin/env bash
# Builds the working tree on a Windows VM over SSH: a snapshot commit of the tree as it is (uncommitted and untracked
# files included, the real index and branches untouched) is pushed to the VM's repo and built there by build.ps1,
# like the dev build CI. Incremental: only what changed since the last build recompiles.
# Usage: tools/vm-build/build.sh [-c CONF] [--quick] [--package] [--rebuild] [--no-build] [--fetch] [--install DEST] [--deck]
#   -c CONF          build configuration (default DX11-AVX)
#   --quick          a test build without LTCG (a one-file change links in a fraction of the time, the exes are a
#                    little slower: never for benchmarks or releases), in its own checkout on the VM ($NEXTRAY_VM_DIR
#                    with -quick appended) so neither build recompiles everything when switching. Give it to
#                    --no-build too, to fetch/install the quick build
#   --package        also stage the CI artifact layout (bin, db/mods) on the VM
#   --no-build       no snapshot and no build: fetch/install what the VM built last (build while the game runs,
#                    then close it and install in seconds; a change relinks both exes, 5-16 min with LTCG)
#   --fetch          copy the result back to _build/vm/ (package/ with --package, else the exes, their .sym files
#                    (crash reports), nvngx_dlss.dll and the XeSS DLLs)
#   --install DEST   --fetch, then install into the game folder DEST (the one with anomaly/ and gamma/), a local
#                    path or user@host:/path over SSH: the exes and DLLs (DLSS, XeSS) to anomaly/bin (the replaced ones
#                    kept in backup-nextray-exes-prev/), gamedata/ to anomaly/gamedata (the shader patches in it too:
#                    the neXt-Ray performance patches MO2 mod of earlier builds is emptied, its shaders now stale).
#                    Can be given more than once (--install-to is the same)
#   --deck           --install $NEXTRAY_DECK (default deck@192.168.0.230:/home/deck/Games/gamma)
# Env: NEXTRAY_VM (default windows@192.168.64.4), NEXTRAY_VM_DIR (default C:/nr), NEXTRAY_DECK

set -euo pipefail

VM=${NEXTRAY_VM:-windows@192.168.64.4}
DIR=${NEXTRAY_VM_DIR:-C:/nr}
QUICK=0
OLD_MOD="neXt-Ray performance patches" # its shaders moved to gamedata/shaders/nextray
CONF=DX11-AVX
PS_ARGS=()
FETCH=0
BUILD=1
PACKAGE=0
TARGETS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -c) CONF=$2; shift ;;
        --quick) PS_ARGS+=(-Quick); QUICK=1 ;;
        --package) PS_ARGS+=(-Package); PACKAGE=1 ;;
        --rebuild) PS_ARGS+=(-Rebuild) ;;
        --fetch) FETCH=1 ;;
        --no-build) BUILD=0 ;;
        --install|--install-to) [ $# -ge 2 ] || { echo "$1 needs the game folder" >&2; exit 2; }; TARGETS+=("${2%/}"); shift ;;
        --deck) TARGETS+=("${NEXTRAY_DECK:-deck@192.168.0.230:/home/deck/Games/gamma}") ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
[ ${#TARGETS[@]} -gt 0 ] && FETCH=1
[ $QUICK = 1 ] && DIR=$DIR-quick

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
# Never replace the files of a running game (checked again right before installing)
game_running() {
    if [ -n "$(target_host "$1")" ]; then ssh -o LogLevel=ERROR "$(target_host "$1")" "pgrep -f -i 'anomalydx[a-z0-9]*\.exe'" >/dev/null
    else pgrep -f -i 'anomalydx[a-z0-9]*\.exe' >/dev/null; fi
}

if [ $BUILD = 1 ]; then
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
fi

if [ $FETCH = 1 ]; then
    rm -rf _build/vm && mkdir -p _build/vm
    if [ $PACKAGE = 1 ]; then
        scp -q -o LogLevel=ERROR -r "$VM:$DIR/package" _build/vm/
        bin=_build/vm/package/bin
    else
        # The crash reports' symbol files, when the build wrote them
        scp -q -o LogLevel=ERROR "$VM:$DIR/_build/_game/bin_dbg/*.sym" _build/vm/ 2>/dev/null || true
        scp -q -o LogLevel=ERROR "$VM:$DIR/_build/_game/bin_dbg/*.exe" \
            "$VM:$DIR/src/3rd party/NVIDIA-DLSS/lib/Windows_x86_64/rel/nvngx_dlss.dll" \
            "$VM:$DIR/src/3rd party/Intel-XeSS/bin/*.dll" _build/vm/
        bin=_build/vm
    fi
    echo "Fetched to _build/vm/"
fi

for t in ${TARGETS[@]+"${TARGETS[@]}"}; do
    if game_running "$t"; then echo "the game is running on $t: close it first, nothing installed" >&2; exit 1; fi
    echo "--- installing to $t"
    names=$(cd "$bin" && { ls *.exe *.dll; ls *.sym 2>/dev/null || true; } | tr '\n' ' ')
    on_target "$t" "rm -rf backup-nextray-exes-prev && mkdir backup-nextray-exes-prev &&
        for f in $names; do if [ -f anomaly/bin/\$f ]; then cp -p anomaly/bin/\$f backup-nextray-exes-prev/; fi; done"
    # -c: by content, timestamps differ between checkouts; -t so later runs list only real changes
    rsync -ct "$bin"/*.exe "$bin"/*.dll $(ls "$bin"/*.sym 2>/dev/null || true) "$(at "$t" anomaly/bin/)"
    on_target "$t" "cd anomaly/bin && chmod 644 $names"
    echo "bin: $names"
    rsync -rct --itemize-changes gamedata/ "$(at "$t" anomaly/gamedata/)" | sed 's/^/gamedata: /'
    on_target "$t" "if [ -d 'gamma/mods/$OLD_MOD/gamedata' ]; then rm -rf 'gamma/mods/$OLD_MOD/gamedata'; echo 'old patch mod: emptied (its shaders are in gamedata now)'; fi"
    echo "Installed to $t (replaced exes in backup-nextray-exes-prev/)"
done
