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
    subprocess.run(command+['install','--no-start','--binary',str(binary)],env=env,check=True)
    installed=home/'.local/bin/fileward'; config=home/'config/fileward/fileward.conf'
    unit=home/'config/systemd/user/fileward.service'
    assert installed.exists() and config.exists() and unit.exists()
    assert '@BINARY@' not in unit.read_text()
    custom=config.read_text()+'# сохранённая настройка\n'
    config.write_text(custom)
    subprocess.run(command+['install','--no-start','--binary',str(binary)],env=env,check=True)
    assert config.read_text()==custom
    assert subprocess.check_output([str(installed),'--version']).strip()
    # systemd проверяет именно сгенерированный unit, включая экранирование путей.
    subprocess.run(['systemd-analyze','--user','verify',str(unit)],env=env,check=True)
    config.write_text('invalid config\n')
    previous=installed.read_bytes()
    result=subprocess.run(command+['install','--no-start','--binary',str(binary)],env=env,capture_output=True)
    assert result.returncode!=0 and installed.read_bytes()==previous
    subprocess.run(command+['uninstall','--no-start'],env=env,check=True)
    assert config.exists() and not installed.exists() and not unit.exists()
print('installer checks passed')
