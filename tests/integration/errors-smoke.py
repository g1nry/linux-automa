#!/usr/bin/env python3
"""Причины отказов, контекст и однострочный UTF-8 в stderr без действующего systemd."""
import base64
import errno
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/fileward').resolve())
with tempfile.TemporaryDirectory(prefix='fw-errors-') as folder:
    root = Path(folder)
    inbox = root / 'inbox'; inbox.mkdir()
    private = inbox / 'private'; private.mkdir()
    target = root / 'target'; target.mkdir()
    (target / 'copy.txt').write_text('preserved')
    config = root / 'fileward.conf'; journal = root / 'events.jsonl'; log = root / 'run.log'
    helper = root / 'helper.py'
    helper.write_text('import os,signal\nos.kill(os.getpid(), signal.SIGTERM)\n')
    process = None

    def rows():
        return [json.loads(line) for line in journal.read_bytes().splitlines(keepends=True)
                if line.endswith(b'\n')] if journal.exists() else []

    def has(code, name=None):
        return any(r['code'] == code and (name is None or r['relative_path'] == name) for r in rows())

    def wait(predicate):
        end = time.monotonic() + 8
        while time.monotonic() < end:
            if predicate(): return
            assert process.poll() is None, log.read_text()
            time.sleep(.02)
        raise AssertionError(log.read_text())

    def publish(name):
        staged = root / 'staged'; staged.write_text('test')
        os.rename(os.fsencode(staged), os.fsencode(inbox) + b'/' + os.fsencode(name))

    def stop():
        if process and process.poll() is None:
            process.terminate()
            assert process.wait(timeout=5) == 0, log.read_text()

    config.write_text(f'''watch {inbox}
rule bad priority=10 stop name=bad* when moved_to ** exec /bin/false DO-NOT-LOG-THIS-ARGUMENT
rule missing priority=10 stop name=missing* when moved_to ** exec /not/a/program
rule signal priority=10 stop name=signal* when moved_to ** exec {sys.executable} {helper}
rule timeout priority=10 stop name=timeout* timeout=1s when moved_to ** exec /bin/sleep 5
rule copy priority=10 stop name=copy* conflict=fail when moved_to ** copy {target}
rule ready when created ** log
''')
    with log.open('w') as output:
        process = subprocess.Popen([binary, 'run', '--config', str(config), '--journal', str(journal),
                                    '--settle-ms', '20', '--ready-timeout-ms', '300'], stdout=output, stderr=output)
        try:
            wait(lambda: any(r['kind'] == 'daemon_started' for r in rows()))
            bad_bytes = 'bad-"\nкириллица-'.encode() + b'\xff.txt'
            bad_name = os.fsdecode(bad_bytes)
            for name in (bad_name, 'missing.txt', 'signal.txt', 'timeout.txt', 'copy.txt'):
                publish(name)
            for code in ('FW_EXEC_EXIT', 'FW_EXEC_START', 'FW_EXEC_SIGNAL', 'FW_EXEC_TIMEOUT', 'FW_DESTINATION_EXISTS'):
                wait(lambda: any(r['kind'] == 'action_result' and r['code'] == code for r in rows()))
            with (inbox / 'held.txt').open('w') as writer:
                writer.write('held'); writer.flush()
                wait(lambda: has('FW_READY_TIMEOUT', 'held.txt'))
            if os.geteuid() != 0:
                with (private / 'denied.txt').open('w') as writer:
                    writer.write('held'); writer.flush()
                    wait(lambda: any(r['kind'] == 'file_observed' and r['relative_path'] == 'private/denied.txt'
                                     for r in rows()))
                    private.chmod(0)
                    try:
                        wait(lambda: has('FW_PERMISSION_DENIED', 'private/denied.txt'))
                    finally:
                        private.chmod(0o700)
            # Wait for the stderr mirror too: JSON is published before the text line.
            wait(lambda: 'FW_READY_TIMEOUT:' in log.read_text())
        finally:
            stop()
    text = log.read_text()  # Strict UTF-8; journald must not receive binary garbage.
    lines = text.splitlines()
    assert 'DO-NOT-LOG-THIS-ARGUMENT' not in text and 'No child processes' not in text, text
    assert not any(line.startswith('<3>') for line in lines), 'Terminal output has journal-only prefixes'
    failures = [line for line in lines if 'kind="action_result"' in line]
    assert len(failures) == 5, text
    for line in failures:
        for field in ('watch_root=', 'relative_path=', 'event=', 'rule=', 'action=', 'attempt=1', 'event_id=', 'parent_id='):
            assert field in line, line
    exited = next(line for line in failures if 'FW_EXEC_EXIT:' in line)
    assert 'exit_code=1' in exited and 'executable="/bin/false"' in exited, exited
    assert '\\nкириллица-\\xff.txt' in exited and '\\"' in exited, exited
    assert 'errno=' not in exited, exited
    signalled = next(line for line in failures if 'FW_EXEC_SIGNAL:' in line)
    assert 'signal=15' in signalled and 'signal_name=' in signalled, signalled
    missing = next(line for line in failures if 'FW_EXEC_START:' in line)
    assert 'errno=2' in missing and 'executable="/not/a/program"' in missing, missing
    copied = next(line for line in failures if 'FW_DESTINATION_EXISTS:' in line)
    assert f'destination="{target}/copy.txt"' in copied and 'errno=17' in copied, copied
    readiness = next(line for line in lines if 'FW_READY_TIMEOUT:' in line)
    assert 'timeout_ms=300' in readiness and 'settle_ms=20' in readiness, readiness
    row = next(r for r in rows() if r['kind'] == 'action_result' and r['code'] == 'FW_EXEC_EXIT')
    assert row['errno'] == 0 and base64.b64decode(row['path_base64']) == bad_bytes, row
    if os.geteuid() != 0:
        denied = next(line for line in lines if 'FW_PERMISSION_DENIED:' in line)
        assert 'errno=13' in denied and 'private/denied.txt' in denied, denied
    assert (target / 'copy.txt').read_text() == 'preserved'

    config.write_text(f'watch {inbox}\nwhen moved_to ** exec /bin/sleep 10\n')
    with log.open('w') as output:
        process = subprocess.Popen([binary, 'run', '--config', str(config), '--journal', str(journal),
                                    '--workers', '1', '--queue-size', '1', '--ready-mode', 'immediate'],
                                   stdout=output, stderr=output)
        try:
            wait(lambda: 'fileward started' in log.read_text())
            for n in range(6): publish(f'pressure-{n}')
            wait(lambda: 'FW_QUEUE_FULL:' in log.read_text())
        finally:
            stop()
    lines = log.read_text().splitlines()
    queue = next(line for line in lines if 'FW_QUEUE_FULL:' in line)
    assert 'capacity=1' in queue and 'queued=' in queue and 'active=' in queue, queue
    assert not any(': error:' in line and 'FW_CANCELLED' in line for line in lines), lines

    # After --log-file, an inherited JOURNAL_STREAM no longer describes stderr.
    occupied = root / 'occupied.sock'; occupied.write_text('keep')
    redirected = root / 'redirected.log'
    with (root / 'original-stderr.log').open('w') as output:
        st = os.fstat(output.fileno())
        env = dict(os.environ, JOURNAL_STREAM=f'{st.st_dev}:{st.st_ino}')
        result = subprocess.run([binary, 'run', '--config', str(config), '--journal', str(journal),
                                 '--control-socket', str(occupied), '--log-file', str(redirected)],
                                stdout=output, stderr=output, env=env, timeout=4)
    assert result.returncode == 3 and 'FW_CONTROL_LISTEN:' in redirected.read_text()
    assert '<3>' not in redirected.read_text(), redirected.read_text()
print('stderr error causes, exec status, context, queue/readiness and escaping passed')
