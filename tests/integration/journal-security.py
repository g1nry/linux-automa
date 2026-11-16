#!/usr/bin/env python3
"""Права и ссылки журналов, блокировка при закрытом stdin; только временные данные."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/fileward').resolve())
with tempfile.TemporaryDirectory(prefix='fw-private-journal-') as folder:
    root = Path(folder); inbox = root / 'inbox'; inbox.mkdir()
    config = root / 'config'; config.write_text(f'watch {inbox}\nwhen created ** log\n')
    for which in ('journal', 'lock'):
        for kind in ('public', 'group', 'hardlink', 'symlink', 'fifo'):
            case = root / f'{which}-{kind}'; case.mkdir()
            journal = case / 'events.jsonl'
            target = journal if which == 'journal' else Path(str(journal) + '.lock')
            original = case / 'original'; original.write_bytes(b'untouched\n'); original.chmod(0o600)
            if kind == 'symlink': target.symlink_to(original)
            elif kind == 'hardlink': os.link(original, target)
            elif kind == 'fifo': os.mkfifo(target, 0o600)
            else:
                target.write_bytes(b'untouched\n'); target.chmod(0o644 if kind == 'public' else 0o660)
            result = subprocess.run([binary, 'run', '--config', str(config), '--journal', str(journal)],
                                    capture_output=True, timeout=4)
            assert result.returncode == 3 and b'FW_JOURNAL_OPEN' in result.stderr, (which, kind, result)
            assert original.read_bytes() == b'untouched\n'
            if kind not in ('symlink', 'fifo'): assert target.read_bytes() == b'untouched\n'
    journal = root / 'daemon.jsonl'; pidfile = root / 'daemon.pid'; log = root / 'daemon.log'
    try:
        result = subprocess.run([binary, 'start', '--config', str(config), '--journal', str(journal),
                                 '--pidfile', str(pidfile), '--log-file', str(log)], capture_output=True,
                                preexec_fn=lambda: os.close(0), timeout=6)
        assert result.returncode == 0, (result.stderr, log.read_text() if log.exists() else '')
        competing = subprocess.run([binary, 'run', '--config', str(config), '--journal', str(journal)],
                                   capture_output=True, timeout=4)
        assert competing.returncode == 3 and b'FW_JOURNAL_OPEN' in competing.stderr, competing
        assert journal.stat().st_mode & 0o777 == 0o600
    finally:
        if pidfile.exists():
            subprocess.run([binary, 'stop', '--pidfile', str(pidfile)], capture_output=True, timeout=7)
print('private journals, links and daemon lock with closed stdin passed')
