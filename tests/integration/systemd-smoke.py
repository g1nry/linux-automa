#!/usr/bin/env python3
"""Проверка настоящего systemd: readiness, reload, сбой и watchdog в отдельном unit."""
import importlib.util
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tempfile
import time

repo = Path(__file__).resolve().parents[2]
binary = Path(sys.argv[1] if len(sys.argv) > 1 else repo / 'build/fileward').resolve()
spec = importlib.util.spec_from_file_location('installer', repo / 'scripts/manage-user.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


def ctl(*args, check=True):
    return subprocess.run(['systemctl', '--user', *args], text=True, capture_output=True, check=check)


def wait_for(callback, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = callback()
        if result:
            return result
        time.sleep(.05)
    raise AssertionError('Истекло ожидание сервиса')


ctl('show-environment')
unit_name = f'fileward-smoke-{os.getpid()}.service'
with tempfile.TemporaryDirectory(prefix='fw-systemd $ %-') as folder:
    root = Path(folder)
    inbox = root / 'inbox'; inbox.mkdir()
    config = root / 'fileward.conf'
    good = f'watch {inbox}\nwhen created ** log "service verified"\n'
    config.write_text(good)
    control = root / 'control.sock'
    service_binary = root / "fileward $ %"
    shutil.copy2(binary, service_binary)
    settings = dict(binary=service_binary, config=config, journal=root / 'events.jsonl', control=control)
    unit = root / unit_name
    # Короткое окно только для проверки; штатный unit использует 20 секунд.
    unit.write_text(installer.render(settings).replace('WatchdogSec=20s', 'WatchdogSec=2s\nWatchdogSignal=SIGKILL'))
    subprocess.run(['systemd-analyze', '--user', 'verify', str(unit)], check=True)

    def health():
        result = subprocess.run([str(binary), 'health', '--socket', str(control)], capture_output=True, text=True)
        return json.loads(result.stdout) if result.returncode == 0 else None

    def restarted(pid):
        status = health()
        return status if status and status['pid'] != pid else None

    try:
        ctl('link', str(unit))
        ctl('start', unit_name)
        first = wait_for(health)
        assert first['config_generation'] == 1
        time.sleep(2.5)  # Несколько периодов heartbeat не вызывают ложного рестарта.
        assert health()['pid'] == first['pid']
        ctl('reload', unit_name)
        assert health()['config_generation'] == 2
        config.write_text('invalid config\n')
        assert ctl('reload', unit_name, check=False).returncode != 0
        assert health()['config_generation'] == 2
        assert ctl('is-active', unit_name).stdout.strip() == 'active'
        config.write_text(good)
        os.kill(first['pid'], signal.SIGKILL)
        second = wait_for(lambda: restarted(first['pid']))
        assert second['config_generation'] == 1
        # Остановленный главный цикл перестаёт посылать heartbeat: manager перезапускает его.
        os.kill(second['pid'], signal.SIGSTOP)
        third = wait_for(lambda: restarted(second['pid']))
        (inbox / 'after-recovery').write_text('complete')
        wait_for(lambda: health() and health()['chains_completed'] > 0)
        assert third['status'] == 'healthy'
        records = [json.loads(line) for line in settings['journal'].read_text().splitlines()]
        assert any(r['kind'] == 'action_result' and r['outcome'] == 'success' for r in records)
        ctl('stop', unit_name)
        assert not control.exists()
        # Синтаксически верный watch может быть недоступен. Причина должна быть
        # в journald и JSONL основного процесса, даже когда ExecStartPre успешен.
        unit.write_text(unit.read_text().replace('Restart=on-failure', 'Restart=no'))
        ctl('daemon-reload')
        blocked = root / 'blocked'; blocked.mkdir()
        failures = [(root / 'missing/inbox', 2)]
        if os.geteuid() != 0:
            blocked.chmod(0)
            failures.append((blocked, 13))
        try:
            for watch, error in failures:
                config.write_text(f'watch {watch}\nwhen created ** log\n')
                ctl('reset-failed', unit_name, check=False)
                assert ctl('start', unit_name, check=False).returncode != 0
                output = subprocess.check_output(
                    ['journalctl', '--user', '-u', unit_name, '--no-pager', '-n', '100'], text=True)
                assert f'FW_WATCHER_START: {watch}:' in output and os.strerror(error) in output, output
                records = [json.loads(line) for line in settings['journal'].read_text().splitlines()]
                assert any(r['code'] == 'FW_WATCHER_START' and r['watch_root'] == str(watch)
                           and r['errno'] == error and str(watch) in r['message'] for r in records)
        finally:
            blocked.chmod(0o700)
    except BaseException:
        print(ctl('status', unit_name, check=False).stdout, file=sys.stderr)
        print(subprocess.run(['journalctl', '--user', '-u', unit_name, '-n', '50', '--no-pager'], capture_output=True, text=True).stdout, file=sys.stderr)
        raise
    finally:
        ctl('stop', unit_name, check=False)
        ctl('disable', unit_name, check=False)
        ctl('reset-failed', unit_name, check=False)
        ctl('daemon-reload', check=False)
print('systemd readiness, reload, crash restart and watchdog checks passed')
