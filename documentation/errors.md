# Error and status codes

Symbolic codes are the stable contract; numeric errno is the local Linux detail.
An error has outcome=failure. Some FW_* codes describe a deliberate skip/cancellation.
Messages can change without changing codes. Exit codes remain 0 success, 1 usage/path,
2 configuration, 3 runtime/journal. Startup failures that cannot write JSON use coded stderr.

| Code | Meaning |
| --- | --- |
| `FW_CANCELLED` | Active operation cooperatively cancelled. |
| `FW_CONFIG_INVALID` | Initial configuration rejected. |
| `FW_CONFLICT_SKIP` | Explicit skip policy preserved existing output. |
| `FW_DESTINATION_EXISTS` | fail policy encountered a collision. |
| `FW_DIAGNOSTIC_ERROR` | Human diagnostic counterpart; inspect linked typed records for specific cause. |
| `FW_DIRECTORY_REWATCH` | Subtree detached/re-added; no replay of preexisting entries. |
| `FW_DISK_FULL` | No space/quota. |
| `FW_EVENT_COALESCED` | Pending modification refreshed; raw observation remains recorded. |
| `FW_EXEC_EXIT` | Command exited nonzero; exit_code contains the status. |
| `FW_EXEC_FAILED` | Other process supervision failure. |
| `FW_EXEC_SIGNAL` | Command terminated by signal. |
| `FW_EXEC_START` | posix_spawn could not start executable; errno identifies cause. |
| `FW_EXEC_TIMEOUT` | Command exceeded timeout. |
| `FW_FD_LIMIT` | Descriptor resources exhausted. |
| `FW_FILE_NOT_FOUND` | File gone or source path missing. |
| `FW_INOTIFY_OVERFLOW` | Kernel queue overflow; observation gap. |
| `FW_INVALID_ARGUMENT` | Invalid path/template/self-copy or operation argument. |
| `FW_IO_ERROR` | Other OS I/O error; inspect errno and message. |
| `FW_JOURNAL_OPEN` | Cannot open/lock journal, invalid type/symlink or incomplete tail. |
| `FW_JOURNAL_READ` | History/stats could not read or validate retained records. |
| `FW_JOURNAL_SYNC` | Final fsync failed. |
| `FW_JOURNAL_WRITE` | Append/rotation/sync failed; runtime stops (stderr if journal unavailable). |
| `FW_MEMORY_LIMIT` | Allocation/resource limit. |
| `FW_OK` | Normal observation or successful operation. |
| `FW_OUTPUT_INSIDE_WATCH` | Journal/log/state path would observe itself. |
| `FW_PATH_FILTERED` | ignore/deny/allow excluded this event. |
| `FW_PATH_TOO_LONG` | Expanded path/argument exceeds bounds. |
| `FW_PERMISSION_DENIED` | Insufficient permissions. |
| `FW_QUEUE_FULL` | Bounded queue saturated; this observation was not executed. |
| `FW_READY_TIMEOUT` | File never met readiness conditions within the configured deadline. |
| `FW_RELOAD_REJECTED` | Reload invalid or root changed; old snapshot retained. |
| `FW_RESOURCE_BUSY` | Transient busy condition. |
| `FW_RULE_MISMATCH` | Rule disabled or conditions did not match; see message. |
| `FW_RUNTIME_FAILED` | Runtime stopped after a fatal error. |
| `FW_SHUTDOWN` | Pending job cancelled during shutdown. |
| `FW_SOURCE_CHANGED` | Source changed during copying; bounded retry may follow. |
| `FW_SOURCE_REPLACED` | The observed inode was replaced, or source changed after publication. |
| `FW_STATE_WRITE` | Compatibility TSV could not be appended. |
| `FW_SYMLINK_REJECTED` | Operation does not follow source symlink. |
| `FW_UNSUPPORTED_OBJECT` | Directory/FIFO/device or unsupported move source. |
| `FW_WATCHER_IO` | Watcher loop stopped after I/O/callback error. |
| `FW_WATCHER_START` | Initial watcher setup failed. |
| `FW_WATCH_ADD` | Could not add a directory watch; coverage degraded. |
| `FW_WATCH_LIMIT` | 512-directory application limit reached. |
| `FW_WATCH_LOST` | Root removed/renamed/unmounted; runtime exits. |
| `FW_WORKER_START` | Worker-pool initialization failed. |
