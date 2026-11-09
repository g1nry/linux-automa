# Verification

Run `make check` from the repository root. It builds the binary, runs four C suites
(glob, parser/rules, action executor, audit failure handling), six process-level smoke
scenarios and XML/Shuffle-adapter fixture checks. Tests use isolated temporary journals;
they do not write to your default journal, deploy services, or contact a SIEM.

The v0.9 scenario verifies:
- holding a writer open beyond settle time, then copying its complete content after close;
- live observed JSON records before actions, schema field types, Unicode/control/invalid bytes;
- event IDs, parent linkage, destination, command exit details, history and retained counters;
- slow exec alongside another path, reload while work is active, immutable generation IDs;
- per-path serialization and concurrent publication of 60 identical basenames by 8 workers;
- root symlink retargeting, root watch loss and directory descriptor cleanup across 280 deletions;
- queue saturation, readiness deadlines, cancellation and restart;
- record rotation, malformed queries, partial-tail startup rejection and single-writer lock;
- actual RLIMIT_FSIZE write failure followed by runtime exit 3;
- rejection of observation feedback from journals placed inside the watched root.

ASan/UBSan and warnings-as-errors (also configured in CI):

```sh
meson setup build-asan -Db_sanitize=address,undefined -Dwerror=true
meson compile -C build-asan
meson test -C build-asan --print-errorlogs
python3 tests/integration/observability-smoke.py ./build-asan/fileward
python3 tests/integration/actions-smoke.py ./build-asan/fileward
```

ThreadSanitizer (requires a compatible host/kernel/runtime mapping):

```sh
meson setup build-tsan -Db_sanitize=thread -Dwerror=true
meson compile -C build-tsan
python3 tests/integration/observability-smoke.py ./build-tsan/fileward
```

Instrumented stress tests can expose races on exercised paths; they are not a proof of
absence of races. Wazuh decoder/rule execution, real collector rotation/reconnect behavior,
Shuffle workflow execution/authentication and delivery deduplication require an actual
stand. Local fixture tests do not establish that end-to-end result. See integrations/README.md.
