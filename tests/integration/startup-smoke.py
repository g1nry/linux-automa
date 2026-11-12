#!/usr/bin/env python3
"""Проверка preflight и одинаковой причины отказа в stderr и JSONL."""
import errno
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/fileward').resolve())
with tempfile.TemporaryDirectory(prefix='fw-startup-') as folder:
    root = Path(folder)
    config = root / 'fileward.conf'

    def failure(path, error):
        config.write_text(f'watch {path}\nwhen created ** log\n')
        syntax = subprocess.run([binary, 'validate', '--config', str(config)], capture_output=True)
        assert syntax.returncode == 0, syntax.stderr
        preflight = subprocess.run([binary, 'validate', '--check-watch', '--config', str(config)],
                                   capture_output=True)
        journal = root / f'{error}.jsonl'
        result = subprocess.run([binary, 'run', '--config', str(config), '--journal', str(journal)],
                                capture_output=True, timeout=4)
        for result_check in (preflight, result):
            assert result_check.returncode == 3, result_check
            assert b'FW_WATCHER_START' in result_check.stderr, result_check.stderr
            assert os.fsencode(path) in result_check.stderr, result_check.stderr
            assert os.strerror(error).encode() in result_check.stderr, result_check.stderr
        rows = [json.loads(line) for line in journal.read_text().splitlines()]
        row = next(r for r in reversed(rows) if r['code'] == 'FW_WATCHER_START')
        assert row['watch_root'] == str(path) and row['errno'] == error, row
        assert str(path) in row['message'] and os.strerror(error) in row['message'], row
        assert row['event_type'] == '' and row['config_generation'] == 1, row
        assert not any(r['kind'] == 'daemon_started' for r in rows), rows
        assert not Path(str(journal) + '.sock').exists()

    failure(root / 'missing' / 'inbox', errno.ENOENT)
    alias = root / 'missing-link'; alias.symlink_to(root / 'missing-target', target_is_directory=True)
    failure(alias, errno.ENOENT)
    file = root / 'ordinary-file'; file.write_text('x')
    failure(file, errno.ENOTDIR)
    blocked = root / 'blocked'; blocked.mkdir()
    child = blocked / 'inbox'; child.mkdir()
    if os.geteuid() != 0:
        for mode, path in ((0, child), (0o400, child), (0o100, child), (0, blocked)):
            blocked.chmod(0o700); child.chmod(0o700)
            path.chmod(mode)
            try:
                failure(child, errno.EACCES)
            finally:
                blocked.chmod(0o700); child.chmod(0o700)
    else:
        print('Проверка EACCES пропущена: root обходит права каталогов')
    config.write_text(f'watch {child}\nwhen created ** log\n')
    subprocess.run([binary, 'validate', '--config', str(config), '--check-watch'], check=True)
    result = subprocess.run([binary, 'run', '--config', str(config), '--journal', str(root / 'log.jsonl'),
                             '--log-file', str(root / 'absent/fileward.log')], capture_output=True, timeout=4)
    assert result.returncode == 3 and b'FW_LOG_OPEN' in result.stderr, result.stderr
    rows = [json.loads(line) for line in (root / 'log.jsonl').read_text().splitlines()]
    assert any(r['code'] == 'FW_LOG_OPEN' and r['errno'] == errno.ENOENT
               and r['watch_root'] == str(child) for r in rows), rows
    for args in ([], ['--check-watch'], ['--config'], ['--config', str(config), '--unknown'],
                 ['--config', str(config), '--check-watch', '--check-watch']):
        assert subprocess.run([binary, 'validate', *args], capture_output=True).returncode == 1, args
print('startup and watch preflight checks passed')
