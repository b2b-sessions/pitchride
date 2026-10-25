#!/usr/bin/env bash
#
# Run two Mixxx instances on this computer, one leader and one follower, with
# the leader's autopilot performing a scripted set, then compare their logs.
#
#   tools/b2b/run_two_mixxx.sh TRACK_A TRACK_B [SECONDS]
#
# Environment:
#   MIXXX        path to the mixxx binary (default: build/mixxx)
#   OUT          output folder (default: b2b-test-output)
#   HEADLESS=1   use a virtual display (Xvfb) and the JACK dummy driver, for
#                machines without a screen or sound card
#   SIM          fake network for both peers, e.g. "20 6 3" = 20 ms delay,
#                6 ms jitter, 3 % loss (default: none); an optional fourth
#                number caps each side's upload in KB/s, with a queue in front
#                like a mobile link ("40 5 0.5 250")
#   FOLLOWER_HAS which tracks the follower has a copy of: "AB" (default), "A",
#                "B" or "" - missing ones are sent over by the leader
#   TURNS=1      take turns: the host hands over after 30 s; the guest, once
#                live, swaps deck 1 to TRACK_C (which only it has) and makes
#                moves; 25 s later the host asks for control back and gets it
#                (the guest allows taking without asking). Needs TRACK_C.
#   QUIT=1       close both windows at the end (clean exit) and check that the
#                follower's session cache was wiped
#   CODE=1       connect with a join code: starts a local rendezvous server
#                (../b2b-server/server.py, or SERVER_PY) unless SERVER=host:port
#                is given, and the guest joins with the host's code
#   B2B_CUE_TEST=1 (with TURNS=1): instead of the set, press CUE over and over
#                on stopped decks, live and listening, and log the cue points
#                ("cue check" lines in each mixxx.log)
#   B2B_GRID_TEST=1 (with GUEST_AUTOPILOT=moves, FOLLOWER_HAS=""): the host
#                fixes deck 1's beat grid by hand, then the listening guest
#                adjusts its borrowed copy; "grid check" lines in each
#                mixxx.log must agree
#   GUEST_AUTOPILOT=moves  the guest logs its checks (loops, effects, deck 3)
#                while listening, without taking turns
#   RELAY=1      with CODE=1: skip the direct route and go through the server's
#                relay, as behind strict NAT
#   TOGETHER=1   "a deck each": the host plays TRACK_A on deck 1, the guest
#                TRACK_B on deck 2 (synced), both make moves at set times and
#                record what they hear; the report compares the two
#                recordings and when each move was heard on each side
#
# The follower finds the tracks by fingerprint in its own folder: the script
# copies them there under different names to prove matching by content.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MIXXX="${MIXXX:-$ROOT/build/mixxx}"
OUT="${OUT:-$ROOT/b2b-test-output}"
TRACK_A="$(realpath "$1")"
TRACK_B="$(realpath "$2")"
SECONDS_TO_RUN="${3:-60}"
read -r SIM_DELAY SIM_JITTER SIM_LOSS SIM_KBPS <<< "${SIM:-0 0 0 0}"

mkdir -p "$OUT"/leader "$OUT"/follower "$OUT"/follower-music
rm -f "$OUT"/*.csv "$OUT"/*.log "$OUT"/follower-music/*
FOLLOWER_HAS="${FOLLOWER_HAS-AB}"
[[ "$FOLLOWER_HAS" == *A* ]] && cp "$TRACK_A" "$OUT/follower-music/copy-of-A.${TRACK_A##*.}"
[[ "$FOLLOWER_HAS" == *B* ]] && cp "$TRACK_B" "$OUT/follower-music/copy-of-B.${TRACK_B##*.}"

PIDS=()
cleanup() {
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done
}
trap cleanup EXIT

if [[ "${HEADLESS:-0}" == "1" ]]; then
  export DISPLAY=:99
  if ! pgrep -f "Xvfb :99" >/dev/null; then
    rm -f /tmp/.X99-lock
    Xvfb :99 -screen 0 1600x1000x24 >/dev/null 2>&1 & PIDS+=($!)
    sleep 1
  fi
  if ! pgrep -x jackd >/dev/null; then
    jackd -d dummy -r 44100 -p 256 >/dev/null 2>&1 & PIDS+=($!)
    sleep 1
  fi
  for role in leader follower; do
    cat > "$OUT/$role/soundconfig.xml" <<'EOF'
<SoundManagerConfig api="JACK Audio Connection Kit" samplerate="44100" latency="5" sync_buffers="2" force_network_clock="0" deck_count="2">
 <SoundDevice name="system" portAudioIndex="0">
  <output type="Master" index="0" channel="0" channel_count="2"/>
 </SoundDevice>
</SoundManagerConfig>
EOF
  done
  # No waveform drawing: software OpenGL in a virtual display eats the CPU the
  # audio needs.
  for role in leader follower; do
    cfg="$OUT/$role/mixxx.cfg"
    if [[ -f "$cfg" ]] && grep -q "^WaveformType" "$cfg"; then
      sed -i 's/^WaveformType .*/WaveformType 0/' "$cfg"
    else
      printf '\n[Waveform]\nWaveformType 0\nFrameRate 10\n' >> "$cfg"
    fi
  done
  # Skip Mixxx's first-run "choose music library directory" dialog: seed the
  # library with a folder if a database exists, otherwise dismiss the dialog.
  if command -v sqlite3 >/dev/null; then
    [[ -f "$OUT/leader/mixxxdb.sqlite" ]] && sqlite3 "$OUT/leader/mixxxdb.sqlite" \
      "insert or ignore into directories values('$(dirname "$TRACK_A")');"
    [[ -f "$OUT/follower/mixxxdb.sqlite" ]] && sqlite3 "$OUT/follower/mixxxdb.sqlite" \
      "insert or ignore into directories values('$OUT/follower-music');"
  fi
  ( for _ in $(seq 1 120); do
      for w in $(xdotool search --name "Choose music library directory" 2>/dev/null); do
        xdotool key --window "$w" Escape 2>/dev/null || true
      done
      sleep 0.5
    done ) & PIDS+=($!)
fi

# Listening delay: fixed 150 ms for the classic report, measured when taking turns.
[[ "${TURNS:-0}" == "1" ]] && DEFAULT_DELAY=0 || DEFAULT_DELAY=150
COMMON=(B2B_PASSWORD=TEST-PASS-WORD B2B_DELAY_MS="${DELAY_MS:-$DEFAULT_DELAY}" B2B_SIM_DELAY_MS="$SIM_DELAY" B2B_SIM_JITTER_MS="$SIM_JITTER"
        B2B_SIM_LOSS="$SIM_LOSS" B2B_SIM_KBPS="${SIM_KBPS:-0}" B2B_LOG_AFTER_S="$((SECONDS_TO_RUN - 2))")

HOST_TURNS=() GUEST_TURNS=(B2B_AUTOPILOT="${GUEST_AUTOPILOT:-}")
HOST_PILOT="$TRACK_A|$TRACK_B"
if [[ "${TOGETHER:-0}" == "1" ]]; then
  COMMON+=(B2B_PLAY_MODE=deck_each)
  # The guest gets deck 2 from the start, unless trying the + / GIVE flow.
  [[ "${OWNERS_UI:-0}" == "1" ]] || COMMON+=(B2B_GUEST_DECKS=2,4)
  HOST_PILOT="together-host|$TRACK_A"
  HOST_TURNS+=(B2B_MUSIC_DIR="$(dirname "$TRACK_B")") # the host has the guest's track too
  GUEST_TURNS=(B2B_AUTOPILOT="together-guest|$OUT/follower-music/copy-of-B.${TRACK_B##*.}")
  rm -rf "$OUT/rec-leader" "$OUT/rec-follower"
  mkdir -p "$OUT/rec-leader" "$OUT/rec-follower"
  for role in leader follower; do
    printf '\n[Recording]\nDirectory %s\nEncoding WAV\n' "$OUT/rec-$role" >> "$OUT/$role/mixxx.cfg"
  done
fi
if [[ "${TURNS:-0}" == "1" ]]; then
  TRACK_C="$(realpath "$TRACK_C")"
  HOST_TURNS=(B2B_AUTOPILOT_HANDOVER_S=30 B2B_AUTOPILOT_REQUEST_S=25)
  GUEST_TURNS=(B2B_AUTOPILOT="moves|$TRACK_C" B2B_ALLOW_TAKE=1)
fi

HOST_CODE=() GUEST_PEER=(B2B_PEER=127.0.0.1:9000)
if [[ "${CODE:-0}" == "1" ]]; then
  if [[ -z "${SERVER:-}" ]]; then
    SERVER=127.0.0.1:7798
    python3 "${SERVER_PY:-$ROOT/../b2b-server/server.py}" --bind 127.0.0.1 --port 7798 ${SERVER_ARGS:-} > "$OUT/server.log" 2>&1 &
    SERVER_PID=$!
    trap 'kill $SERVER_PID 2>/dev/null' EXIT
    sleep 0.5
  fi
  rm -f "$OUT/code.txt"
  HOST_CODE=(B2B_USE_CODE=1 B2B_SERVER="$SERVER" B2B_CODE_FILE="$OUT/code.txt" B2B_RELAY_ONLY="${RELAY:-0}")
fi

env "${COMMON[@]}" "${HOST_TURNS[@]}" "${HOST_CODE[@]}" B2B_ROLE=leader B2B_PORT=9000 B2B_LOG="$OUT/leader.csv" \
  B2B_AUTOPILOT="$HOST_PILOT" \
  "$MIXXX" --settingsPath "$OUT/leader" --resourcePath "$ROOT/res" --log-flush-level info \
  > "$OUT/leader.log" 2>&1 & PIDS+=($!)
sleep 2
if [[ "${CODE:-0}" == "1" ]]; then
  for _ in $(seq 1 30); do [[ -s "$OUT/code.txt" ]] && break; sleep 0.5; done
  [[ -s "$OUT/code.txt" ]] || { echo "The host never got a join code (see $OUT/leader.log)"; exit 1; }
  # Typed the way a person might.
  GUEST_PEER=(B2B_SERVER="$SERVER" B2B_CODE="$(tr 'A-Z-' 'a-z ' < "$OUT/code.txt")" B2B_RELAY_ONLY="${RELAY:-0}")
  echo "Join code: $(cat "$OUT/code.txt")"
fi
env "${COMMON[@]}" "${GUEST_TURNS[@]}" "${GUEST_PEER[@]}" B2B_ROLE=follower B2B_LOG="$OUT/follower.csv" \
  B2B_MUSIC_DIR="$OUT/follower-music" B2B_FAKE_OFFSET_MS=-237.5 B2B_FAKE_DRIFT_PPM=60 \
  "$MIXXX" --settingsPath "$OUT/follower" --resourcePath "$ROOT/res" --log-flush-level info \
  > "$OUT/follower.log" 2>&1 & PIDS+=($!)

echo "Running for ${SECONDS_TO_RUN}s (logs in $OUT)..."
sleep "$SECONDS_TO_RUN"
for _ in $(seq 1 60); do   # wait for both logs (written B2B_LOG_AFTER_S after start)
  [[ -f "$OUT/leader.csv" && -f "$OUT/follower.csv" ]] && break
  sleep 1
done
sleep 1
if [[ "${QUIT:-0}" == "1" ]]; then
  echo "Follower cache before quitting:"; ls -la "$OUT/follower/b2b-cache" 2>/dev/null || true
  # Quit the follower the way a user would: dismiss any first-run question,
  # Ctrl+Q, then confirm "A deck is currently playing. Exit Mixxx?" (its
  # default button is No, hence Alt+Y).
  FOLLOWER_PID="${PIDS[-1]}"
  for w in $(xdotool search --name "." 2>/dev/null); do
    [[ "$(xdotool getwindowname "$w" 2>/dev/null)" == Allow* ]] && xdotool windowfocus "$w" key Escape 2>/dev/null
  done
  sleep 1
  for w in $(xdotool search --pid "$FOLLOWER_PID" --name "^Mixxx$" 2>/dev/null); do
    xdotool windowfocus "$w" key ctrl+q 2>/dev/null || true
  done
  sleep 2
  for w in $(xdotool search --name "^Confirm Exit$" 2>/dev/null); do
    xdotool windowfocus "$w" key alt+y 2>/dev/null || true
  done
  for _ in $(seq 1 30); do
    kill -0 "$FOLLOWER_PID" 2>/dev/null || break
    sleep 1
  done
  if [[ -d "$OUT/follower/b2b-cache" ]]; then
    echo "Follower cache after quitting: STILL THERE"; ls -la "$OUT/follower/b2b-cache"
  else
    echo "Follower cache after quitting: removed"
  fi
fi
grep -h "B2B" "$OUT/leader/mixxx.log" "$OUT/follower/mixxx.log" 2>/dev/null | tail -6 || true

if [[ -f "$OUT/leader.csv" && -f "$OUT/follower.csv" ]]; then
  if [[ "${TOGETHER:-0}" == "1" ]]; then
    python3 "$ROOT/tools/b2b/analyse_together.py" "$OUT"
  elif [[ "${TURNS:-0}" == "1" ]]; then
    python3 "$ROOT/tools/b2b/analyse_turns.py" "$OUT/leader.csv" "$OUT/follower.csv"
  else
    python3 "$ROOT/tools/b2b/analyse_mixxx.py" "$OUT/leader.csv" "$OUT/follower.csv"
  fi
else
  echo "Logs missing: check $OUT/leader.log and $OUT/follower.log"
  exit 1
fi
