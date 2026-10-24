#!/usr/bin/env bash
set -euo pipefail

binary="$(realpath "${1:-./build/fileward}")"
workdir="$(mktemp -d)"
pidfile="$workdir/fileward.pid"
logfile="$workdir/fileward.log"
statefile="$workdir/fileward.state"
config="$workdir/fileward.conf"

cleanup() {
  if "$binary" status --pidfile "$pidfile" | grep -q 'running'; then
    "$binary" stop --pidfile "$pidfile" || true
    if "$binary" status --pidfile "$pidfile" | grep -q 'running'; then
      kill -KILL "$(cat "$pidfile")" 2>/dev/null || true
    fi
  fi
  rm -rf -- "$workdir"
}
trap cleanup EXIT

wait_for_text() {
  local file="$1"
  local expected="$2"
  for _ in {1..50}; do
    if grep -Fq -- "$expected" "$file" 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  cat "$logfile" >&2
  echo "Timed out waiting for: $expected" >&2
  return 1
}

start_daemon() (
  cd "$workdir"
  "$binary" start --config fileward.conf --pidfile fileward.pid --log-file fileward.log --state-file fileward.state
)

mkdir "$workdir/watch"
printf 'watch watch\nwhen created *.txt log "daemon rule"\n' > "$config"
start_daemon
"$binary" status --pidfile "$pidfile" | grep -q 'running'
if start_daemon; then
  echo 'Duplicate daemon start unexpectedly succeeded' >&2
  exit 1
fi

touch "$workdir/watch/first.txt"
wait_for_text "$logfile" 'action log: daemon rule'
wait_for_text "$statefile" $'first.txt\tlog'

printf 'watch watch\nwhen created *.md log "reloaded daemon rule"\n' > "$config"
"$binary" reload --pidfile "$pidfile"
wait_for_text "$logfile" 'reloaded config:'
touch "$workdir/watch/second.md"
wait_for_text "$logfile" 'action log: reloaded daemon rule'
wait_for_text "$statefile" $'second.md\tlog'

"$binary" stop --pidfile "$pidfile"
"$binary" status --pidfile "$pidfile" | grep -q 'stopped'

start_daemon
"$binary" status --pidfile "$pidfile" | grep -q 'running'
"$binary" stop --pidfile "$pidfile"
"$binary" status --pidfile "$pidfile" | grep -q 'stopped'

printf 'watch %s/missing\n' "$workdir" > "$config"
if start_daemon; then
  echo 'Daemon startup unexpectedly succeeded for a missing watch directory' >&2
  exit 1
fi
"$binary" status --pidfile "$pidfile" | grep -q 'stopped'
echo 'daemon smoke test passed'
