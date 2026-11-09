#!/usr/bin/env bash
set -euo pipefail

binary="${1:-./build/fileward}"
workdir="$(mktemp -d)"
pid=""

cleanup() {
  if [[ -n "$pid" ]]; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  rm -rf -- "$workdir"
}
trap cleanup EXIT

wait_for_log() {
  local expected="$1"
  for _ in {1..50}; do
    if grep -Fq -- "$expected" "$workdir/fileward.log"; then
      return 0
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      break
    fi
    sleep 0.1
  done
  cat "$workdir/fileward.log" >&2
  echo "Timed out waiting for: $expected" >&2
  return 1
}

mkdir "$workdir/watch"
printf 'watch %s/configured-watch\nwhen created *.txt log "text rule"\n' "$workdir" > "$workdir/fileward.conf"
: > "$workdir/fileward.log"
"$binary" run --config "$workdir/fileward.conf" --log-file "$workdir/fileward.log" "$workdir/watch" > "$workdir/launcher.log" 2>&1 &
pid=$!
wait_for_log 'fileward started'

touch "$workdir/watch/first.txt"
wait_for_log 'action log: text rule'

printf 'watch %s/configured-watch\nwhen created *.md log "markdown rule"\n' "$workdir" > "$workdir/fileward.conf"
kill -HUP "$pid"
wait_for_log 'reloaded config:'
touch "$workdir/watch/second.md"
wait_for_log 'action log: markdown rule'

printf 'when created *.md log "invalid rule"\n' > "$workdir/fileward.conf"
kill -HUP "$pid"
wait_for_log 'keeping previous rules'
touch "$workdir/watch/third.md"
for _ in {1..50}; do
  if [[ "$(grep -Fc 'action log: markdown rule' "$workdir/fileward.log")" -ge 2 ]]; then
    echo 'service smoke test passed'
    exit 0
  fi
  sleep 0.1
done
cat "$workdir/fileward.log" >&2
echo 'Previous rules stopped working after invalid reload' >&2
exit 1
