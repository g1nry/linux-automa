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
  local text="$1"
  for _ in {1..50}; do
    if grep -Fq -- "$text" "$workdir/run.log"; then return 0; fi
    if ! kill -0 "$pid" 2>/dev/null; then break; fi
    sleep 0.1
  done
  cat "$workdir/run.log" >&2
  echo "Timed out waiting for: $text" >&2
  exit 1
}
stop_process() {
  kill "$pid"
  wait "$pid"
  pid=""
}
start_process() {
  : > "$workdir/run.log"
  "$binary" run --journal "$workdir/events.jsonl" --config "$workdir/rules.conf" --log-file "$workdir/run.log" --state-file "$workdir/state" "$@" &
  pid=$!
  wait_for_log 'fileward started'
}
prepare_pdf() {
  truncate -s 2048 "$workdir/$1"
  touch -d '2 minutes ago' "$workdir/$1"
}

mkdir "$workdir/watch"
config="$workdir/rules.conf"
conditions='min-size=1KiB max-size=4KiB min-age=60s max-age=1h name=report* extension=pdf type=file'
cat > "$config" <<CONFIG
watch $workdir/watch
mode all
allow **/*.pdf
ignore **/ignored*
deny **/private*
rule low priority=-1 when moved_to ** log "LOW"
rule mover priority=30 $conditions when moved_to ** move $workdir/moved
rule snapshot priority=20 $conditions when moved_to ** log "SNAPSHOT"
rule tieA priority=10 when moved_to ** log "A"
rule tieB priority=10 when moved_to ** log "B"
rule terminal stop when moved_to ** log "STOP"
CONFIG
prepare_pdf sample.pdf
mv "$workdir/sample.pdf" "$workdir/watch/report-preview.pdf"
"$binary" explain --config "$config" --event moved_to "$workdir/watch/report-preview.pdf" > "$workdir/explain"
awk '/rule: mover / { a=NR } /rule: snapshot / { b=NR } /rule: tieA / { c=NR } /rule: tieB / { d=NR } END { if (!(a<b && b<c && c<d && a>0)) exit 1 }' "$workdir/explain"
grep -Fq 'Rule low (line 6): skipped: rule chain stopped' "$workdir/explain"
"$binary" test --config "$config" "$workdir/watch/report-preview.pdf" > "$workdir/test"
if grep -q '^Match ' "$workdir/test"; then exit 1; fi # moved_to differs from created.

for pair in 'ignored.pdf:path ignored' 'private.pdf:path denied' 'other.txt:path not in allow list'; do
  name="${pair%%:*}"
  reason="${pair#*:}"
  touch "$workdir/watch/$name"
  "$binary" explain --config "$config" --event moved_to "$workdir/watch/$name" > "$workdir/filter"
  grep -Fq "$reason" "$workdir/filter"
done

start_process
for name in ignored-new.pdf private-new.pdf other-new.txt; do
  prepare_pdf "$name"
  mv "$workdir/$name" "$workdir/watch/$name"
done
prepare_pdf report-run.pdf
mv "$workdir/report-run.pdf" "$workdir/watch/report-run.pdf"
wait_for_log 'action log: STOP'
test -f "$workdir/moved/report-run.pdf"
# Metadata conditions continue matching after the preceding move.
awk '/action log: SNAPSHOT/ { a=NR } /action log: A$/ { b=NR } /action log: B$/ { c=NR } /action log: STOP$/ { d=NR } END { if (!(a<b && b<c && c<d && a>0)) exit 1 }' "$workdir/run.log"
if grep -Eq 'action log: LOW|matched (ignored-new|private-new|other-new)' "$workdir/run.log"; then exit 1; fi
for name in ignored-new.pdf private-new.pdf other-new.txt; do test -f "$workdir/watch/$name"; done

# Invalid condition ranges reject filters and rules atomically on reload.
cat > "$config" <<CONFIG
watch $workdir/watch
ignore **
rule invalid min-size=4 max-size=1 when moved_to ** log
CONFIG
kill -HUP "$pid"
wait_for_log 'keeping previous rules'
prepare_pdf report-retained.pdf
mv "$workdir/report-retained.pdf" "$workdir/watch/report-retained.pdf"
wait_for_log "moved '$workdir/watch/report-retained.pdf'"

# Reload mode, filters and ordering together. First-match stops even if its action fails.
printf 'not a directory\n' > "$workdir/not-directory"
cat > "$config" <<CONFIG
watch $workdir/watch
mode first
rule fallback priority=-1 when moved_to ** log "FALLBACK"
rule failing priority=1 when moved_to ** move $workdir/not-directory
CONFIG
kill -HUP "$pid"
wait_for_log 'reloaded config:'
printf 'data\n' > "$workdir/failure.txt"
mv "$workdir/failure.txt" "$workdir/watch/failure.txt"
wait_for_log 'rule="failing"'
stop_process
if grep -Fq 'action log: FALLBACK' "$workdir/run.log"; then exit 1; fi
test -f "$workdir/watch/failure.txt"

# Dry-run preserves matched files and never records successful actions.
cat > "$config" <<CONFIG
watch $workdir/watch
mode first
rule mover type=file min-size=1 when moved_to ** move $workdir/dry-destination
rule fallback when moved_to ** log "DRY-FALLBACK"
CONFIG
cp "$workdir/state" "$workdir/state-before"
start_process --dry-run
printf 'dry\n' > "$workdir/dry.txt"
mv "$workdir/dry.txt" "$workdir/watch/dry.txt"
wait_for_log 'dry-run: would move'
stop_process
test -f "$workdir/watch/dry.txt"
test ! -e "$workdir/dry-destination"
cmp "$workdir/state" "$workdir/state-before"
if grep -Fq 'DRY-FALLBACK' "$workdir/run.log"; then exit 1; fi

# Basename and metadata checks describe symlinks themselves, including dangling ones.
mkdir "$workdir/outside"
ln -s "$workdir/missing-target" "$workdir/watch/link.pdf"
ln -s "$workdir/outside" "$workdir/watch/escape"
cat > "$config" <<CONFIG
watch $workdir/watch
rule link type=symlink extension=pdf when created ** log
rule gone min-size=1 when deleted ** log
rule deletion when deleted ** log
CONFIG
"$binary" test --config "$config" "$workdir/watch/link.pdf" > "$workdir/link"
grep -Fq 'rule: link ' "$workdir/link"
"$binary" explain --config "$config" --event deleted "$workdir/watch/removed.pdf" > "$workdir/deleted"
grep -Fq 'metadata unavailable' "$workdir/deleted"
grep -Fq 'rule: deletion ' "$workdir/deleted"
if "$binary" explain --config "$config" "$workdir/watch/escape/removed.pdf" > "$workdir/escape" 2>&1; then exit 1; fi
grep -Fq 'outside watch root' "$workdir/escape"

# The watch root can itself be configured through a symlink alias.
ln -s "$workdir/watch" "$workdir/watch-alias"
cat > "$config" <<CONFIG
watch $workdir/watch-alias
rule root type=directory when created ** log
CONFIG
"$binary" test --config "$config" "$workdir/watch-alias" > "$workdir/root-alias"
grep -Fq 'rule: root ' "$workdir/root-alias"

# Filters also apply when there are no rules (raw event output mode).
cat > "$config" <<CONFIG
watch $workdir/watch
ignore **/hidden*
CONFIG
start_process
printf 'filtered\n' > "$workdir/watch/hidden.txt"
mkdir "$workdir/watch/fence"
wait_for_log "watching directory: $workdir/watch/fence"
stop_process
if grep -Fq 'path: hidden.txt' "$workdir/run.log"; then exit 1; fi
grep -Fq 'path: fence' "$workdir/run.log"

echo 'v0.7 rules smoke test passed'
