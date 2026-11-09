#!/usr/bin/env bash
set -euo pipefail

binary="$(realpath "${1:-./build/fileward}")"
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
    if grep -Fq -- "$expected" "$workdir/run.log"; then return 0; fi
    if ! kill -0 "$pid" 2>/dev/null; then break; fi
    sleep 0.1
  done
  cat "$workdir/run.log" >&2
  echo "Timed out waiting for: $expected" >&2
  exit 1
}

mkdir "$workdir/watch"
config="$workdir/rules.conf"
cat > "$config" <<CONFIG
watch $workdir/watch
rule paused disabled when created *.txt move $workdir/moved
rule first when created *.txt log "first action"
rule second enabled when created *.txt log "second action"
rule edits when modified *.txt log "edited"
rule pdfs when created *.pdf log "pdf"
CONFIG

# Validation checks syntax without requiring an existing watch root or running actions.
sed "s|$workdir/watch|$workdir/missing|" "$config" > "$workdir/absent.conf"
"$binary" validate --config "$workdir/absent.conf" > "$workdir/validate.out"
grep -Fq 'rules: 5 (4 enabled, 1 disabled)' "$workdir/validate.out"
test ! -e "$workdir/missing"
test ! -e "$workdir/moved"
if "$binary" validate > "$workdir/usage.out" 2>&1; then exit 1; else test "$?" -eq 1; fi
"$binary" validate --help > "$workdir/help.out" 2>&1

touch "$workdir/watch/sample.txt"
"$binary" explain --config "$config" "$workdir/watch/sample.txt" > "$workdir/explain.out"
grep -Fq 'Rule paused (line 2): skipped: disabled' "$workdir/explain.out"
grep -Fq 'Rule edits (line 5): skipped: event does not match' "$workdir/explain.out"
grep -Fq 'Rule pdfs (line 6): skipped: pattern does not match' "$workdir/explain.out"
test "$(grep -c '^Match ' "$workdir/explain.out")" -eq 2
"$binary" test --config "$config" "$workdir/watch/sample.txt" > "$workdir/test.out"
test "$(grep -c '^Match ' "$workdir/test.out")" -eq 2
if grep -Fq 'paused' "$workdir/test.out"; then exit 1; fi

touch "$workdir/outside.txt"
for command in test explain; do
  if "$binary" "$command" --config "$config" "$workdir/outside.txt" > "$workdir/outside.out" 2>&1; then
    echo "$command accepted a path outside watch root" >&2
    exit 1
  fi
  grep -Fq 'outside watch root' "$workdir/outside.out"
done

: > "$workdir/run.log"
"$binary" run --config "$config" --log-file "$workdir/run.log" > "$workdir/launcher.log" 2>&1 &
pid=$!
wait_for_log 'fileward started'
touch "$workdir/watch/first.txt"
wait_for_log 'action log: second action'
test -e "$workdir/watch/first.txt"
test ! -e "$workdir/moved"
# All matches run in config order; disabled move must never execute.
awk '/action log: first action/ { first = NR } /action log: second action/ { if (!first || first >= NR) exit 1; seen = 1 } END { if (!seen) exit 1 }' "$workdir/run.log"

# Duplicate names reject the whole reload, preserving the previous rules.
printf 'watch %s/watch\nrule same when created * log\nrule same when created * log\n' "$workdir" > "$config"
if "$binary" validate --config "$config" > "$workdir/invalid.out" 2>&1; then exit 1; else test "$?" -eq 2; fi
grep -Fq "$config:3: duplicate rule name" "$workdir/invalid.out"
kill -HUP "$pid"
wait_for_log 'keeping previous rules'
touch "$workdir/watch/retained.txt"
wait_for_log "rule 'second' (line 4) matched retained.txt"

# A valid reload can enable a previously disabled rule.
cat > "$config" <<CONFIG
watch $workdir/watch
rule paused enabled when created *.txt move $workdir/moved
CONFIG
kill -HUP "$pid"
wait_for_log 'reloaded config:'
touch "$workdir/watch/move.txt"
wait_for_log "moved '$workdir/watch/move.txt'"
test -e "$workdir/moved/move.txt"
test ! -e "$workdir/watch/move.txt"

echo 'rules smoke test passed'
