#!/usr/bin/env bash
set -euo pipefail

binary="${1:-./build/fileward}"
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

wait_for_log() {
  local expected="$1"
  for _ in {1..50}; do
    if grep -Fq -- "$expected" "$logfile"; then
      return 0
    fi
    sleep 0.1
  done
  cat "$logfile" >&2
  echo "Timed out waiting for: $expected" >&2
  return 1
}

mkdir "$workdir/watch"
printf 'watch %s/watch\nwhen created *.txt log "daemon rule"\n' "$workdir" > "$config"
"$binary" start --config "$config" --pidfile "$pidfile" --log-file "$logfile" --state-file "$statefile"
"$binary" status --pidfile "$pidfile" | grep -q 'running'
if "$binary" start --config "$config" --pidfile "$pidfile" --log-file "$logfile"; then
  echo 'Duplicate daemon start unexpectedly succeeded' >&2
  exit 1
fi

touch "$workdir/watch/first.txt"
wait_for_log 'action log: daemon rule'
grep -Fq $'first.txt\tlog' "$statefile"

"$binary" stop --pidfile "$pidfile"
"$binary" status --pidfile "$pidfile" | grep -q 'stopped'

"$binary" start --config "$config" --pidfile "$pidfile" --log-file "$logfile"
"$binary" status --pidfile "$pidfile" | grep -q 'running'
"$binary" stop --pidfile "$pidfile"
"$binary" status --pidfile "$pidfile" | grep -q 'stopped'

printf 'watch %s/missing\n' "$workdir" > "$config"
if "$binary" start --config "$config" --pidfile "$pidfile" --log-file "$logfile"; then
  echo 'Daemon startup unexpectedly succeeded for a missing watch directory' >&2
  exit 1
fi
"$binary" status --pidfile "$pidfile" | grep -q 'stopped'
echo 'daemon smoke test passed'
