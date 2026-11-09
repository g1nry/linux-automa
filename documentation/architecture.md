# Runtime architecture (v0.9)

`src/app` owns CLI and daemon lifecycle. `src/watcher` owns inotify descriptors and the
watch table. `src/core` parses immutable configurations and evaluates rules.
`src/runtime` schedules bounded per-path event chains; `src/actions` executes one action.
`src/observability` owns diagnostics, JSONL serialization, retention, and offline queries.
Public interfaces live in `include/fileward`. Tests are split into unit/integration;
SIEM templates live in `integrations`. `third_party/jsmn` is pinned MIT-licensed code used
only to tokenize history records (commit 25647e692c7906b96ffd2b05ca54c097948e879c).

## Execution

The observation thread receives kernel events and writes file_observed immediately,
including filtered/coalesced events. It never executes an action. A bounded queue holds
readiness state and a refcounted immutable config snapshot. Default capacity: 256,
including active work. Excess events are explicitly recorded as FW_QUEUE_FULL; no
unbounded memory growth or hidden fallback to inline execution occurs.

Four workers by default execute independent paths concurrently. For one relative path,
pending and active chains are serialized across configuration generations. Rules inside
a chain retain stable descending priorities and all/first/stop semantics. Different paths
can identify hardlinks or write the same configured destination: destination conflict
policies still arbitrate atomically. Configured actions must account for that concurrency.

Reload allocates/publishes a new snapshot without waiting for active actions. Existing
pending/active jobs retain the configuration they observed; new jobs use the new generation.
The root is pinned to its canonical absolute path; retargeting a configuration symlink cannot redirect active jobs. The resolved root directory cannot change on reload. Allocation/parse failures preserve the old
configuration. Every structured record identifies its generation. Snapshots are freed
when their final job completes. Queue capacity also bounds the number of retained versions.

Workers inherit blocked INT/TERM/HUP. The observation thread handles these signals and
publishes cancellation through a C11 atomic flag. Shutdown cancels pending jobs, stops
active actions cooperatively, joins workers, writes the final event and syncs/closes the
journal before releasing the pidfile lock. Blocking kernel/filesystem calls can still delay
shutdown; cancellation is not an interruptible-I/O guarantee.

Exec uses posix_spawn with a new process group, explicit default signals and empty signal
mask, /dev/null stdin and closefrom(3). No non-async-safe child setup runs after fork in a
multithreaded process. glibc >= 2.34 is required. Timeouts/shutdown terminate the owned group.
Commands that deliberately escape the group are not supervised by this mechanism.

## Readiness

Default `--ready-mode close-write`: regular created/modified files require IN_CLOSE_WRITE
and a stable size/mtime/ctime for `--settle-ms` (100 ms by default). This prevents a paused
single open writer from being mistaken for a finished download. Moved-in files may already
be closed; they use stability, optionally accelerated by an observed close. Directories,
symlinks, removal events and `--ready-mode immediate` bypass readiness waiting.

`--ready-mode stable` uses stability without requiring close. It is a heuristic: an open
writer can pause longer than the window. Close-write also cannot prove that *all* writers
are finished: one descriptor may close while another remains open. Moved-in files that
continue to be written have the same stability caveat. For strict producer/consumer
workflows, stage outside the watched tree, close, and atomically rename into it.

Default timeout: 300000 ms. Missing/replaced files and expired waits are coded cancellations,
not attempted actions. Pending modifications for one path/config are coalesced; every raw
observation is still in the journal. A recreated inode never inherits the old job's readiness.
Rules inspect metadata once at dispatch, before the first action. Conditions such as min-age
remain filters evaluated at that point, not scheduling timers.

## Limits and failure modes

inotify is not a durable message queue. Kernel overflow and watch failures are explicit;
there is no automatic reconstruction of missed events. Losing/renaming the root exits with
code 3. A moved-out subtree is detached, and a moved-in directory is recursively watched
at its new location. The rewatch window is recorded; preexisting files are not replayed.
There are at most 512 directory watches. Child watch failures are visible and can degrade
coverage; review watcher_error records.

JSONL appends are serialized under a mutex. Local append I/O can briefly delay observation;
slow actions cannot. Use a local disk and an external collector for network delivery. There
is no promised exactly-once processing, crash recovery, or transactional rollback across a
chain. Killing the process can leave action_started without action_result. An audit write
failure stops accepting/executing further work; already performed effects cannot be undone.
The structured record is written before a new action starts. --journal-sync additionally
requests fdatasync per record. Diagnostic stdout/stderr remains separate, including child output.
