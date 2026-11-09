#!/usr/bin/env python3
"""Exercise JSONL, worker isolation, readiness, reload, retention and coded failures."""
import base64
import json
import os
import resource
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/fileward').resolve())


def check(condition, message):
    if not condition:
        raise AssertionError(message)


with tempfile.TemporaryDirectory(prefix='fileward-v09-') as temporary:
    root = Path(temporary)
    inbox = root / 'inbox'
    inbox.mkdir()
    config = root / 'config'
    journal = root / 'events.jsonl'
    log = root / 'run.log'
    helper = root / 'helper.py'
    helper.write_text('''import sys,time
from pathlib import Path
if sys.argv[1] == "sleep": time.sleep(float(sys.argv[2]))
elif sys.argv[1] == "record":
    with Path(sys.argv[2]).open("a") as f: f.write(sys.argv[3]+"\\n")
''')
    process = None
    stream = None

    def records():
        result = []
        for suffix in ('.3', '.2', '.1', ''):
            file = Path(str(journal) + suffix)
            if file.exists():
                lines = file.read_bytes().splitlines(keepends=True)
                for line in lines:
                    if line.endswith(b'\n'):
                        result.append(json.loads(line))
        return result

    def wait(predicate, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            if process and process.poll() is not None:
                raise AssertionError(log.read_text())
            time.sleep(.02)
        raise AssertionError('timeout\n' + log.read_text())

    def write_config(rules):
        config.write_text(f'watch {inbox}\n' + rules)

    def start(*options):
        global process, stream
        stream = log.open('w')
        process = subprocess.Popen([binary, 'run', '--config', str(config), '--journal', str(journal),
                                    '--settle-ms', '50', *options], stdout=stream, stderr=stream)
        wait(lambda: 'fileward started' in log.read_text())

    def stop(expected=0):
        global process, stream
        if process:
            process.terminate()
            try:
                check(process.wait(timeout=5) == expected, log.read_text())
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(); raise
            finally:
                process = None
                stream.close(); stream = None

    def has(kind, name=None, code=None, generation=None):
        return any(r['kind'] == kind and (name is None or r['relative_path'] == name) and
                   (code is None or r['code'] == code) and (generation is None or r['config_generation'] == generation)
                   for r in records())

    try:
        # Pause an open writer for longer than settle-ms: created must not copy yet.
        write_config(f'rule copy when created ** copy {root}/copies\n')
        start('--workers', '3', '--journal-sync')
        duplicate = subprocess.run([binary, 'run', '--config', str(config), '--journal', str(journal)], capture_output=True, timeout=3)
        check(duplicate.returncode == 3 and b'FW_JOURNAL_OPEN' in duplicate.stderr, 'journal must have one writer')
        source = inbox / 'slow.txt'
        with source.open('wb') as writer:
            writer.write(b'first'); writer.flush()
            wait(lambda: has('file_observed', 'slow.txt'))
            time.sleep(.25)
            check(not (root / 'copies/slow.txt').exists(), 'open writer was processed prematurely')
            check(not has('action_started', 'slow.txt'), 'created must await close_write')
            writer.write(b'-last')
        wait(lambda: (root / 'copies/slow.txt').exists())
        check((root / 'copies/slow.txt').read_bytes() == b'first-last', 'full contents after readiness')
        wait(lambda: has('action_result', 'slow.txt'))
        stop()
        rows = records()
        schema = json.loads((Path(__file__).resolve().parents[2] / 'documentation/events.schema.json').read_text())
        for row in rows:
            check(set(schema['required']) <= set(row), 'missing required schema fields')
            for key, definition in schema['properties'].items():
                if 'const' in definition: check(row[key] == definition['const'], key)
                types = definition.get('type', [])
                if isinstance(types, str): types = [types]
                if types:
                    check(any((t == 'null' and row[key] is None) or (t == 'string' and isinstance(row[key], str)) or
                              (t == 'integer' and type(row[key]) is int) or (t == 'boolean' and type(row[key]) is bool)
                              for t in types), 'wrong type: ' + key)
        ids = [row['event_id'] for row in rows]
        check(len(ids) == len(set(ids)), 'IDs must be unique')
        result = next(r for r in rows if r['kind'] == 'action_result')
        check(result['parent_id'] in ids and result['destination'].endswith('/copies/slow.txt'), 'result linkage/destination')
        check(result['duration_ms'] >= 0 and result['config_generation'] == 1, 'typed fields')
        history = subprocess.check_output([binary, 'history', '--journal', str(journal)], text=True)
        check(all(json.loads(line)['kind'] == 'action_result' for line in history.splitlines()), 'history default filtering')
        linked = subprocess.check_output([binary, 'history', '--journal', str(journal), '--all', '--event-id', result['parent_id']], text=True)
        linked_rows = [json.loads(line) for line in linked.splitlines()]
        check(any(r['event_id'] == result['parent_id'] for r in linked_rows), 'history root link')
        check(all(r['event_id'] == result['parent_id'] or r['parent_id'] == result['parent_id'] for r in linked_rows), 'history link filtering')
        stats = json.loads(subprocess.check_output([binary, 'stats', '--journal', str(journal)]))
        check(stats['actions_succeeded'] == 1 and stats['rules_matched'] == 1, stats)

        # A slow exec cannot hold up another file, reception, or reload.
        write_config(f'rule slow name=slow* when moved_to ** exec {sys.executable} {helper} sleep 2\n'
                     'rule fast name=fast* when moved_to ** log "fast completed"\n')
        start('--workers', '3')
        staged = root / 'staged'
        staged.write_text('slow'); staged.rename(inbox / 'slow-job')
        wait(lambda: has('action_started', 'slow-job'))
        write_config('rule new when created ** log "new config"\n')
        process.send_signal(signal.SIGHUP)
        wait(lambda: has('config_reload', code='FW_OK', generation=2), timeout=1)
        (inbox / 'fast-new').write_text('ready')
        wait(lambda: has('action_result', 'fast-new', generation=2), timeout=1)
        check(not has('action_result', 'slow-job'), 'slow action should still be running while fast result arrives')
        wait(lambda: has('action_result', 'slow-job', generation=1))
        stop()

        # Replacing a root symlink cannot redirect an already running executor.
        alias = root / 'alias'
        alias.symlink_to(inbox, target_is_directory=True)
        other = root / 'other-tree'
        other.mkdir()
        config.write_text(f'watch {alias}\nrule pin when created ** copy {root}/pinned-copy\n')
        start()
        alias.unlink(); alias.symlink_to(other, target_is_directory=True)
        (other / 'pinned.txt').write_text('untouched')
        (inbox / 'pinned.txt').write_text('original-root')
        wait(lambda: (root / 'pinned-copy/pinned.txt').exists())
        check((root / 'pinned-copy/pinned.txt').read_text() == 'original-root', 'root symlink redirected source')
        check((other / 'pinned.txt').read_text() == 'untouched', 'wrong root modified')
        process.send_signal(signal.SIGHUP)
        wait(lambda: has('config_reload', code='FW_RELOAD_REJECTED'))
        stop()

        # Multiple event chains for one path must not overlap.
        order_file = root / 'order'
        write_config(f'rule wait priority=1 when modified ** exec {sys.executable} {helper} sleep .15\n'
                     f'rule record when modified ** exec {sys.executable} {helper} record {order_file} done\n')
        start('--ready-mode', 'immediate', '--workers', '4')
        (inbox / 'serial').write_text('1')
        wait(lambda: has('action_started', 'serial'))
        for _ in range(4):
            with (inbox / 'serial').open('a') as f: f.write('2')
            time.sleep(.03)
        wait(lambda: has('event_finished', 'serial'))
        time.sleep(.6)
        stop()
        selected = [r for r in records() if r['relative_path'] == 'serial' and r['kind'] in ('action_started', 'event_finished')]
        action_parents = {r['parent_id'] for r in selected if r['kind'] == 'action_started'}
        active = None
        for row in selected:
            if row['parent_id'] not in action_parents: continue
            if row['kind'] == 'action_started':
                check(active is None or active == row['parent_id'], 'overlapping chains for one path')
                active = row['parent_id']
            else:
                check(active == row['parent_id'], 'chain result ordering'); active = None

        # Error causes and arbitrary filename bytes are structured, not interpolated JSON.
        write_config('rule missing name=missing* when moved_to ** exec /not/a/program\n'
                     'rule failure name=fail* when moved_to ** exec /bin/false\n'
                     'rule timeout name=timeout* timeout=1s when moved_to ** exec /bin/sleep 3\n'
                     'rule rest priority=-1 when moved_to ** log "ok"\n')
        start()
        for name in ('missing-one', 'fail-one', 'timeout-one', 'quotes"\nкириллица.txt'):
            staged.write_text('x'); staged.rename(inbox / name)
        invalid = os.fsencode(inbox) + b'/invalid-\xff.txt'
        fd = os.open(invalid, os.O_CREAT | os.O_WRONLY, 0o600); os.close(fd)
        wait(lambda: has('action_result', 'missing-one', 'FW_EXEC_START'))
        wait(lambda: has('action_result', 'fail-one', 'FW_EXEC_EXIT'))
        wait(lambda: has('action_result', 'timeout-one', 'FW_EXEC_TIMEOUT'))
        wait(lambda: has('file_observed', 'invalid-\ufffd.txt'))
        stop()
        bad = next(r for r in records() if r['kind'] == 'file_observed' and r['relative_path'] == 'invalid-\ufffd.txt')
        check(base64.b64decode(bad['path_base64']) == b'invalid-\xff.txt', 'lossless filename bytes')
        failure = next(r for r in records() if r['code'] == 'FW_EXEC_EXIT' and r['kind'] == 'action_result')
        check(failure['exit_code'] == 1, failure)

        # Concurrent atomic publication into one directory must retain every payload.
        for i in range(60): (inbox / f'group-{i}').mkdir()
        write_config(f'rule copies conflict=rename when moved_to ** copy {root}/collision-output\n')
        start('--workers', '8')
        for i in range(60):
            staged.write_text(str(i)); staged.rename(inbox / f'group-{i}/same.txt')
        wait(lambda: len([r for r in records() if r['kind'] == 'action_result' and r['action'] == 'copy' and r['destination'].startswith(str(root / 'collision-output'))]) == 60)
        stop()
        outputs = list((root / 'collision-output').iterdir())
        check(len(outputs) == 60 and {p.read_text() for p in outputs} == {str(i) for i in range(60)}, 'concurrent collision data loss')

        # Deleted watches must be retired even though IN_IGNORED has no filename.
        write_config('rule dirs type=directory when created ** log\n')
        start('--workers', '4', '--queue-size', '1024')
        for i in range(280): (inbox / f'old-dir-{i}').mkdir()
        wait(lambda: 'watching directory: ' + str(inbox / 'old-dir-279') in log.read_text())
        for i in range(280): (inbox / f'old-dir-{i}').rmdir()
        wait(lambda: has('file_observed', 'old-dir-279') and any(r['event_type'] == 'deleted' and r['relative_path'] == 'old-dir-279' for r in records()))
        for i in range(280): (inbox / f'new-dir-{i}').mkdir()
        wait(lambda: 'watching directory: ' + str(inbox / 'new-dir-279') in log.read_text())
        stop()
        check(not has('watcher_error', code='FW_WATCH_LIMIT'), 'deleted watch descriptors leaked')
        for i in range(280): (inbox / f'new-dir-{i}').rmdir()

        # Queue pressure and readiness timeouts are visible; no unbounded allocation.
        write_config('rule slow when moved_to ** exec /bin/sleep 2\n')
        start('--workers', '1', '--queue-size', '2')
        for i in range(10):
            staged.write_text('x'); staged.rename(inbox / f'pressure-{i}')
        wait(lambda: has('event_dropped', code='FW_QUEUE_FULL'))
        stop()
        write_config('rule text when created ** log\n')
        start('--ready-timeout-ms', '200')
        with (inbox / 'never-closed').open('w') as writer:
            writer.write('x'); writer.flush()
            wait(lambda: has('event_cancelled', 'never-closed', 'FW_READY_TIMEOUT'))
        stop()

        # Restart/rotation retain parseable records and query tools read retained segments.
        write_config('rule log when created ** log\n')
        start('--journal-max-bytes', '4096', '--ready-mode', 'immediate')
        for i in range(30): (inbox / f'rotation-{i}').write_text('x')
        wait(lambda: Path(str(journal) + '.3').exists())
        stop()
        check(records(), 'rotation records missing')
        stats = json.loads(subprocess.check_output([binary, 'stats', '--journal', str(journal)]))
        check(stats['malformed_records'] == 0, stats)
        # Loss of the root watch is fatal and coded, rather than silently going idle.
        start()
        inbox.rename(root / 'renamed-root')
        deadline = time.monotonic() + 5
        while process.poll() is None and time.monotonic() < deadline: time.sleep(.02)
        check(process.poll() == 3, 'root watch loss must fail the daemon')
        stop(expected=3)
        check(has('event_dropped', code='FW_WATCH_LOST'), 'watch-loss diagnostic')
        (root / 'renamed-root').rename(inbox)
        # Real write failure (RLIMIT_FSIZE/EFBIG) must stop the daemon, not run unaudited.
        def file_limit():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (8192, 8192))
        fault_log = root / 'fault.log'
        fault_inbox = root / 'fault-inbox'
        fault_inbox.mkdir()
        fault_config = root / 'fault-config'
        fault_config.write_text(f'watch {fault_inbox}\nwhen created ** log\n')
        with fault_log.open('w') as output:
            fault = subprocess.Popen([binary, 'run', '--config', str(fault_config), '--journal', str(root / 'fault.jsonl'),
                                      '--ready-mode', 'immediate'], stdout=output, stderr=output,
                                     preexec_fn=file_limit, restore_signals=False)
            try:
                deadline = time.monotonic() + 5
                while 'fileward started' not in fault_log.read_text() and time.monotonic() < deadline:
                    if fault.poll() is not None: break
                    time.sleep(.02)
                check(fault.poll() is None, fault_log.read_text())
                for i in range(40): (fault_inbox / f'disk-failure-{i}').write_text('x')
                check(fault.wait(timeout=5) == 3, 'audit write failure must exit 3')
                check('FW_JOURNAL_WRITE' in fault_log.read_text(), 'missing write-failure diagnostic')
            finally:
                if fault.poll() is None: fault.kill(); fault.wait()
        # A partial final record is explicitly rejected on the next startup.
        with journal.open('ab') as file: file.write(b'{"unfinished":')
        result = subprocess.run([binary, 'run', '--config', str(config), '--journal', str(journal)], capture_output=True, timeout=3)
        check(result.returncode == 3 and b'FW_JOURNAL_OPEN' in result.stderr, 'partial-tail startup rejection')
        malformed = root / 'malformed.jsonl'
        malformed.write_text('not json\n')
        invalid_stats = subprocess.run([binary, 'stats', '--journal', str(malformed)], capture_output=True)
        check(invalid_stats.returncode == 3 and json.loads(invalid_stats.stdout)['malformed_records'] == 1, 'malformed query input')
        inside = subprocess.run([binary, 'run', '--config', str(config), '--journal', str(inbox / 'audit.jsonl')], capture_output=True)
        check(inside.returncode == 2 and b'FW_OUTPUT_INSIDE_WATCH' in inside.stderr, 'self-observation must be rejected')
    finally:
        stop()

print('observability smoke test passed')
