#!/usr/bin/env python3
"""Исполнение единственного example.conf в изолированном HOME."""
import gzip
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

repo = Path(__file__).resolve().parents[2]
binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else repo / 'build/fileward').resolve())
with tempfile.TemporaryDirectory(prefix='fw-example-') as folder:
    root = Path(folder)
    lab = root / 'Downloads/FilewardLab'
    inbox = lab / 'inbox'; inbox.mkdir(parents=True)
    staging = lab / 'staging'; staging.mkdir()
    journal = root / 'events.jsonl'
    env = dict(os.environ, HOME=str(root))
    command = [binary, 'run', '--config', str(repo / 'example.conf'), '--journal', str(journal),
               '--settle-ms', '20']

    def rows():
        return [json.loads(line) for line in journal.read_bytes().splitlines(keepends=True)
                if line.endswith(b'\n')] if journal.exists() else []

    def wait(predicate):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if predicate(): return
            assert process.poll() is None, log.read_text()
            time.sleep(.02)
        raise AssertionError(log.read_text())

    def result(rule, name):
        return any(r['kind'] == 'action_result' and r['rule_name'] == rule
                   and r['relative_path'] == name and r['outcome'] == 'success' for r in rows())

    log = root / 'run.log'
    with log.open('w') as output:
        process = subprocess.Popen(command, env=env, stdout=output, stderr=output)
        try:
            wait(lambda: any(r['kind'] == 'daemon_started' for r in rows()))
            # Каталоги создаются при работающем watcher; ждём подписки до публикации.
            for name in ('private', 'scratch'):
                (inbox / name).mkdir()
                wait(lambda: f'watching directory: {inbox / name}' in log.read_text())
            cases = [('important-report.pdf', 1024, 0, 'important-pdf'),
                     ('important-small.pdf', 1023, 0, 'pdf'),
                     ('important-max.pdf', 10 * 1024 * 1024, 0, 'important-pdf'),
                     ('important-large.pdf', 10 * 1024 * 1024 + 1, 0, 'pdf'),
                     ('important-old.pdf', 2048, 8 * 86400, 'pdf'),
                     ('normal.pdf', 20, 0, 'pdf'), ('screenshot-demo.png', 20, 0, 'screenshots'),
                     ('picture.png', 20, 0, 'png'), ('photo.jpg', 20, 0, 'jpg'),
                     ('old.txt', 20, 7200, 'finish'), ('new.txt', 20, 0, 'finish'),
                     ('fallback.zip', 20, 0, 'finish')]
            for name, size, age, rule in cases:
                source = staging / name
                source.write_bytes(b'x' * size)
                if age: os.utime(source, (time.time() - age, time.time() - age))
                inode = source.stat().st_ino
                source.rename(inbox / name)
                wait(lambda: result(rule, name))
                action = next(r for r in rows() if r['kind'] == 'action_result'
                              and r['rule_name'] == rule and r['relative_path'] == name)
                target = Path(action['destination'])
                assert not (inbox / name).exists() and target.stat().st_ino == inode, action
                backup = next(r for r in rows() if r['kind'] == 'action_result'
                              and r['rule_name'] == 'backup' and r['relative_path'] == name)
                assert Path(backup['destination']).read_bytes() == b'x' * size
            assert result('old-text', 'old.txt') and result('gzip', 'old.txt')
            assert not result('old-text', 'new.txt')
            assert gzip.decompress((lab / 'sorted/compressed/old.txt.gz').read_bytes()) == b'x' * 20
            blocked = ['private/blocked.pdf', 'scratch/ignored.pdf', 'ignored.tmp', 'ignored.part', 'unlisted.bin']
            for name in blocked:
                source = staging / 'blocked'; source.write_text('filtered')
                source.rename(inbox / name)
                wait(lambda: any(r['kind'] == 'event_filtered' and r['relative_path'] == name for r in rows()))
                assert (inbox / name).exists()
                assert not any(r['kind'] == 'action_started' and r['relative_path'] == name for r in rows())
            (inbox / 'demo-directory').mkdir()
            wait(lambda: result('new-directory', 'demo-directory'))
            (inbox / 'demo-link').symlink_to(staging / 'absent')
            wait(lambda: result('new-symlink', 'demo-link'))
            removed = inbox / 'demo-removed'; removed.touch()
            wait(lambda: any(r['kind'] == 'file_observed' and r['relative_path'] == removed.name for r in rows()))
            removed.unlink()
            wait(lambda: result('removed', removed.name))
            assert not any(r['kind'] == 'action_result' and r['outcome'] == 'failure' for r in rows())
        finally:
            process.terminate()
            assert process.wait(timeout=5) == 0, log.read_text()
print('example routing, backup, gzip, filters, metadata and object types passed')
