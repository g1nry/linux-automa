#!/usr/bin/env python3
"""Изолированная установка: пути с пробелами, сохранение конфига и обновление."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

repo=Path(__file__).resolve().parents[2]
binary=Path(sys.argv[1] if len(sys.argv)>1 else repo/'build/fileward').resolve()
with tempfile.TemporaryDirectory(prefix='fw home $ %-') as folder:
    home=Path(folder)
    runtime=home/'run'; runtime.mkdir()
    env=dict(os.environ,HOME=str(home),XDG_CONFIG_HOME=str(home/'config'),XDG_STATE_HOME=str(home/'state'),XDG_RUNTIME_DIR=str(runtime))
    command=[sys.executable,str(repo/'scripts/manage-user.py')]
    result=subprocess.run(command+['install','--no-start','--binary',str(binary)],env=env,check=True,
                          capture_output=True,text=True)
    assert 'Создана новая конфигурация:' in result.stdout, result.stdout
    installed=home/'.local/bin/fileward'; config=home/'config/fileward/fileward.conf'
    unit=home/'config/systemd/user/fileward.service'
    assert installed.exists() and config.exists() and unit.exists()
    assert '@BINARY@' not in unit.read_text()
    custom=config.read_text()+'# сохранённая настройка\n'
    config.write_text(custom)
    result=subprocess.run(command+['install','--no-start','--binary',str(binary)],env=env,check=True,
                          capture_output=True,text=True)
    assert 'Используется существующая конфигурация:' in result.stdout, result.stdout
    assert str(config) in result.stdout and str(home/'state/fileward/events.jsonl') in result.stdout
    assert str(home/'Downloads/Fileward/inbox') in result.stdout
    assert config.read_text()==custom
    config.write_text('watch Downloads/Fileward/inbox\nwhen created ** log\n')
    result=subprocess.run(command+['install','--no-start','--binary',str(binary)],env=env,check=True,
                          capture_output=True,text=True)
    assert str(home/'Downloads/Fileward/inbox') in result.stdout
    config.write_text(custom)
    assert subprocess.check_output([str(installed),'--version']).strip()
    # systemd проверяет именно сгенерированный unit, включая экранирование путей.
    subprocess.run(['systemd-analyze','--user','verify',str(unit)],env=env,check=True)
    config.write_text('invalid config\n')
    previous=installed.read_bytes()
    result=subprocess.run(command+['install','--no-start','--binary',str(binary)],env=env,capture_output=True)
    assert result.returncode!=0 and installed.read_bytes()==previous
    config.write_text(f'watch {home}/missing/inbox\nwhen created ** log\n')
    previous_unit=unit.read_bytes()
    result=subprocess.run(command+['install','--no-start','--binary',str(binary)],env=env,capture_output=True)
    assert result.returncode!=0 and b'FW_WATCHER_START' in result.stderr, result.stderr
    assert installed.read_bytes()==previous and unit.read_bytes()==previous_unit
    journal=home/'state/fileward/events.jsonl'; journal.write_text('preserved journal\n')
    subprocess.run(command+['uninstall','--no-start'],env=env,check=True)
    assert config.exists() and not installed.exists() and not unit.exists()
    assert journal.read_text()=='preserved journal\n'
print('installer checks passed')
