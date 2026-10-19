#!/bin/bash
# Drives the game's remote console (engine flag -remote_console) on the Steam Deck (Steam shortcut) or on macOS
# (Sikarugir wrapper app in the game folder, through MO2).
#   remote_console.sh launch [s]         start the game, wait for the main menu (default 300 s)
#   remote_console.sh send <cmd>...     queue console commands (one argument per command)
#   remote_console.sh run <file>        queue the commands in a file
#   remote_console.sh load <save> [s]   load a save from the main menu or a level, wait until it runs (default 600 s)
#   remote_console.sh wait <state> [s]  wait until state is menu / level / idle (default 300 s)
#   remote_console.sh idle [s]          wait until the command queue is empty and no rc_wait is running
#   remote_console.sh screenshot [out]  take a screenshot (end of the frame, UI included), copy it to out (.png/.jpg)
#   remote_console.sh transition <x> <y> <z> [s]
#                                       teleport onto a level changer and travel (without its dialog), wait for the end
#                                       of the new level's precache frames, print the phases' times (default 300 s)
#   remote_console.sh status | log [n] | running | quit
# Input for UI tests (console commands): rc_key <key name> [down|up], rc_click <left|right> [down|up],
# rc_cursor <x> <y> (UI coordinates, 1024x768)
# Env: GAMMA_ROOT (the game folder with anomaly/ and gamma/), GAMMA_SHORTCUT (MO2 executable, macOS, default
# "Anomaly (DX11-AVX)"), GAMMA_GAMEID (Steam shortcut id, Deck)
set -u
if [ "$(uname)" = Darwin ]; then
	ROOT="${GAMMA_ROOT:-/Volumes/SSD/stalker-gamma+mac.arm64}"
	LOGNAME_="xray_crossover.log"
	mtime() { stat -f %m "$1"; }
else
	ROOT="${GAMMA_ROOT:-/home/deck/Games/gamma}"
	LOGNAME_="xray_steamuser.log"
	mtime() { stat -c %Y "$1"; }
fi
APPDATA="${GAMMA_APPDATA:-$ROOT/anomaly/appdata}"
GAMEID="${GAMMA_GAMEID:-12049572732266872832}"   # "stalker gamma" non-Steam shortcut
CMD="$APPDATA/remote_cmd.txt"
STATUS="$APPDATA/remote_status.txt"
LOG="$APPDATA/logs/$LOGNAME_"

running() { pgrep -f -i 'anomalydx[a-z0-9]*\.exe' >/dev/null; }

# Status is rewritten twice a second; older than 5 s means the game is not updating it
status_fresh() { [ -f "$STATUS" ] && [ $(( $(date +%s) - $(mtime "$STATUS") )) -le 5 ]; }

field() { sed -n "s/^$1=//p" "$STATUS" 2>/dev/null | tr -d '\r'; }

send_lines() {
	local tmp="$APPDATA/remote_cmd.$$.tmp"
	# Wait until the game has taken the commands before
	while [ -f "$CMD" ]; do sleep 0.1; done
	cat > "$tmp"
	mv "$tmp" "$CMD"
}

wait_state() {
	local want="$1" timeout="${2:-300}" start=$(date +%s)
	while :; do
		if status_fresh && [ "$(field state)" = "$want" ]; then return 0; fi
		if [ $(( $(date +%s) - start )) -ge "$timeout" ]; then
			echo "timeout waiting for $want (state=$(field state), fresh=$(status_fresh && echo 1 || echo 0))" >&2
			return 1
		fi
		sleep 1
	done
}

case "${1:-}" in
	launch)
		if running; then echo "already running"; else
			rm -f "$STATUS" "$CMD"
			if [ "$(uname)" = Darwin ]; then
				# The wrapper app runs a .bat: MO2 with the executable's shortcut (its arguments hold -remote_console)
				bat="${TMPDIR:-/tmp}/nextray_launch.bat"
				printf '"Z:%s/gamma/ModOrganizer.exe" "moshortcut://:%s"\r\n' "$ROOT" "${GAMMA_SHORTCUT:-Anomaly (DX11-AVX)}" > "$bat"
				app=$(ls -d "$ROOT"/*.app | head -1)
				# Right after a quit the wrapper app and Wine are still exiting: open would only bring that app to the
				# front and the .bat would not run
				for _ in $(seq 1 60); do
					ps -eo args | grep -v grep | grep -q -e "$(basename "$app")" -e wineserver || break
					sleep 1
				done
				open -a "$app" "$bat"
			else
				steam -ifrunning "steam://rungameid/$GAMEID" >/dev/null 2>&1 &
			fi
		fi
		wait_state menu "${2:-300}"
		;;
	send)  shift; printf '%s\n' "$@" | send_lines ;;
	run)   send_lines < "$2" ;;
	load)
		# The "press any key" prompt after loading would stop the game; off for the load only
		printf '%s\n' "keypress_on_start 0" "start server($2/single/alife/load) client(localhost)" \
			"rc_wait_level ${3:-600}" "keypress_on_start 1" | send_lines
		sleep 5
		wait_state level "${3:-600}"
		;;
	transition)
		timeout="${5:-300}"
		start_line=$(( $(wc -l < "$LOG") + 1 ))
		# A menu or the "press any key" prompt would hold the game; changers that ask are answered by
		# rc_silent_level_changers (not saved)
		printf '%s\n' "main_menu off" "keypress_on_start 0" "rc_silent_level_changers 1" "log_timestamps 1" \
			"run_string db.actor:set_actor_position(vector():set($2,$3,$4))" | send_lines
		start=$(date +%s)
		until tail -n +"$start_line" "$LOG" | grep -aq 'Handled Necessary Textures Destruction'; do
			[ $(( $(date +%s) - start )) -ge "$timeout" ] && { echo "timeout waiting for the transition" >&2; exit 1; }
			sleep 1
		done
		sleep 2
		printf '%s\n' "log_timestamps 0" "rc_silent_level_changers 0" "keypress_on_start 1" "flush" | send_lines
		sleep 2
		tail -n +"$start_line" "$LOG" | tr -d '\r' | python3 -c '
import re, sys
marks = [("teleport", r"\[remote\] run_string db\.actor:set_actor_position"), ("saving", r"Saving spawns"),
	("unloading", r"Destroying level"), ("scripts loaded", r"\[axr_main\] scripts loaded in"),
	("collision model", r"CDB: collision model"), ("level loaded", r"phase time"),
	("precache done", r"Handled Necessary Textures Destruction")]
found, last_phase = {}, None
for line in sys.stdin:
	m = re.search(r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]", line)
	if not m:
		continue
	t = int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + int(m[4]) / 1000
	for name, rx in marks:
		if re.search(rx, line):
			if name == "level loaded":
				last_phase = t # the last phase line before the precache frames
			elif name not in found:
				found[name] = (t, line.strip())
	if "precache done" in found and last_phase is not None and "level loaded" not in found:
		found["level loaded"] = (last_phase, "")
t0 = found.get("teleport", (None,))[0]
prev = t0
for name, _ in marks:
	if name in found and t0 is not None:
		t, line = found[name]
		extra = line.split("]", 2)[-1].strip()[:90] if name in ("scripts loaded", "collision model") else ""
		print("%-16s %6.2f s  (+%5.2f)  %s" % (name, t - t0, t - prev, extra))
		prev = t
'
		;;
	wait)  wait_state "$2" "${3:-300}" ;;
	screenshot)
		dir="$APPDATA/screenshots"
		before=$(ls -t "$dir" 2>/dev/null | head -1)
		printf 'screenshot\n' | send_lines
		start=$(date +%s)
		while :; do
			new=$(ls -t "$dir" 2>/dev/null | head -1)
			if [ -n "$new" ] && [ "$new" != "$before" ]; then
				sleep 1 # written in one piece, but give the encoder its time
				break
			fi
			[ $(( $(date +%s) - start )) -ge 30 ] && { echo "no screenshot after 30 s" >&2; exit 1; }
			sleep 0.5
		done
		if [ -n "${2:-}" ]; then
			if [ "$(uname)" = Darwin ] && [ "${2##*.}" != "${new##*.}" ]; then
				sips -s format "$( [ "${2##*.}" = jpg ] && echo jpeg || echo "${2##*.}")" "$dir/$new" --out "$2" >/dev/null
			else
				cp "$dir/$new" "$2"
			fi
			echo "$2"
		else
			echo "$dir/$new"
		fi
		;;
	idle)
		timeout="${2:-600}"; start=$(date +%s)
		sleep 1
		until [ ! -f "$CMD" ] && [ "$(field queue)" = 0 ] && [ "$(field waiting)" = 0 ]; do
			[ $(( $(date +%s) - start )) -ge "$timeout" ] && { echo "timeout" >&2; exit 1; }
			sleep 1
		done
		;;
	status)  status_fresh || echo "(stale)"; cat "$STATUS" 2>/dev/null | tr -d '\r' ;;
	log)     tail -n "${2:-40}" "$LOG" | tr -d '\r' ;;
	running) running && echo yes || { echo no; exit 1; } ;;
	quit)
		running || exit 0
		printf 'quit\n' | send_lines
		for i in $(seq 60); do running || exit 0; sleep 1; done
		echo "still running after 60 s" >&2; exit 1
		;;
	*) sed -n '2,20p' "$0"; exit 1 ;;
esac
