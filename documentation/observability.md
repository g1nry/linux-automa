# Journals, history and counters

All run/start invocations produce JSONL. Default path:
`$XDG_STATE_HOME/fileward/events.jsonl`, or `$HOME/.local/state/fileward/events.jsonl`.
Use an absolute `--journal PATH` for services. Parent directories are created 0700; new
journal/lock files are 0600. Journals, text logs, and legacy state files must be outside
watch. Explicit paths are interpreted relative to the preserved launch directory.

```sh
./build/fileward run --config fileward.conf --journal /tmp/fileward/events.jsonl --workers 4
./build/fileward history --journal /tmp/fileward/events.jsonl --limit 20
./build/fileward history --journal /tmp/fileward/events.jsonl --all --limit 100
./build/fileward history --journal /tmp/fileward/events.jsonl --all --event-id RUN-SEQUENCE
./build/fileward stats --journal /tmp/fileward/events.jsonl
```

History emits the most recent action_result records as JSONL. --all includes the whole
lifecycle, and --event-id includes a record and its direct children (use an observed file's
ID to inspect its chain). Limit is 1..1000, default 50. Stats emits JSON counters for the
retained segments: observations, readiness completions, completed chains, evaluated/matched
rules, successful/skipped/cancelled actions, coded failures, retries, coalescing, dropped/cancelled
jobs and diagnostic errors. A runtime failure and its diagnostic can coexist: failures
excludes diagnostic/exec-detail records to avoid counting one action twice. It is not a
count of unique incidents. Counters are reconstructed from disk, not erased on clean restart.
They cover retained data, not lifetime totals or a transactional live snapshot.

Default rotation is 64 MiB, retaining current + .1/.2/.3. Set --journal-max-bytes from 4096
to 4294967296. A single record may exceed the configured segment size; it is never split by
rotation. The per-process sequence/run_id survives rotation. A separate .lock enforces one
writer per journal path. Separate daemon instances require separate journal paths. Don't
remove/replace that lock while a writer runs, and don't externally rotate the file; use the
built-in retention. Ensure collectors consume faster than segments expire.

Records are written immediately with write(2); there is no stdio output buffer on the
journal. By default fsync occurs on rotation/clean close, so a power loss can lose the last
kernel-buffered records. --journal-sync performs fdatasync per record, trading throughput
for stronger local persistence. Neither mode guarantees remote delivery or exactly-once
processing. Filesystem failure/ENOSPC/short writes stop the runtime with exit 3 and a
FW_JOURNAL_WRITE diagnostic on stderr. The next startup rejects an incomplete tail rather
than silently appending invalid JSON. Preserve the damaged segment for analysis and choose
a new journal path or repair the saved file offline. Read-only queries skip a live incomplete
last line, report malformed complete records, and return 3 on malformed/unreadable input. Queries read a finite size snapshot per segment even while the writer continues appending.

Text diagnostics are human-readable and serialized between threads. A corresponding
`diagnostic` record is JSON-escaped; child stdout/stderr is NOT automatically parsed into
JSON. The legacy --state-file TSV remains success-only and is serialized across workers.
It is not lossless for unusual filenames; JSONL + path_base64 is the authoritative audit.
Dry-run still produces audit records with dry_run=true, but action_result.outcome=dry_run
and no action-success TSV row. Operator shutdown uses cancelled outcomes rather than failure alerts. Program exit codes remain 0/1/2/3 as before.

## Schema version 1

All records are flat JSON objects. The complete machine-readable schema is events.schema.json.
Additive fields may be introduced within a schema version; consumers should ignore unknown
fields and reject unsupported schema versions.

- provider=fileward, program_version: producer identification.
- timestamp: UTC record creation time with milliseconds. This is observation/processing
  time, not the original kernel operation timestamp (inotify does not supply that timestamp).
- event_id: random 128-bit run_id + sequence. Parent_id links rule/action/readiness records
  to the original file_observed ID; lifecycle records may have an empty parent.
- host_id: /etc/machine-id when available, otherwise the run ID; host_name is informational.
- daemon_pid: producer process, NEVER the user/process that modified the file.
- kind/outcome/code: record classification, result and stable symbolic error code.
- event_type: created/modified/deleted/moved_from/moved_to/close_write or watcher status.
- watch_root, relative_path, destination: display strings; destination is the expanded
  actual published path (including a collision suffix) when one was produced.
- path_base64 and destination_base64: exact original path bytes. Valid UTF-8 is kept in
  display fields; invalid bytes become U+FFFD. Newlines, quotes and controls are escaped.
- file_size/file_mode/mtime_unix: lstat snapshot at observation/readiness/dispatch, or null
  when unavailable. Removal events intentionally have no metadata. Original metadata is
  retained throughout a chain, even after a move.
- rule_name/action, config_generation, dry_run, attempt, duration_ms: execution context.
- errno: local OS error number, 0 when inapplicable; exit_code/signal detail exec failures.
- message: explanatory text, not a stable parser contract.

Kinds: file_observed, file_closed, event_filtered, event_coalesced, file_ready,
rule_evaluated, action_started, action_retry, exec_status, action_result, event_finished,
event_cancelled, event_dropped, daemon_started, daemon_stopped, config_load, config_reload,
watcher_error, watcher_gap, runtime_error, state_error, diagnostic.

A successful log action is not itself a security alert. Map explicit failures/policies into
SIEM rules. See ../integrations/README.md for Wazuh and Shuffle templates and validation limits.
