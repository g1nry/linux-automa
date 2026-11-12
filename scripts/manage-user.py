#!/usr/bin/env python3
"""Установка пользовательского сервиса с сохранением конфигурации."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def paths():
    home = Path.home()
    config_home = Path(os.environ.get('XDG_CONFIG_HOME') or home / '.config')
    state_home = Path(os.environ.get('XDG_STATE_HOME') or home / '.local/state')
    runtime = Path(os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}')
    control = runtime / 'fileward/control.sock' if runtime.is_dir() and os.access(runtime, os.W_OK) else state_home / 'fileward/control.sock'
    return {'binary': home / '.local/bin/fileward', 'config': config_home / 'fileward/fileward.conf',
            'journal': state_home / 'fileward/events.jsonl', 'control': control,
            'unit': config_home / 'systemd/user/fileward.service',
            'inbox': home / 'Downloads/Fileward/inbox'}


def systemd_quote(path, execute=True):
    value = str(path)
    if any(ord(c) < 32 for c in value):
        raise ValueError('Путь содержит управляющий символ')
    # Спецификаторы разворачиваются везде; переменные — в аргументах Exec*,
    # но не в первом слове с путём исполняемого файла.
    value = value.replace('\\', '\\\\').replace('"', '\\"').replace('%', '%%')
    if execute:
        value = value.replace('$', '$$')
    return '"' + value + '"'


def render(settings):
    content = (ROOT / 'systemd/user/fileward.service.in').read_text()
    for key in ('binary', 'config', 'journal', 'control'):
        content = content.replace('@' + key.upper() + '@', systemd_quote(settings[key], execute=key != 'binary'))
    return content


def atomic_write(path, data, mode):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.' + path.name + '-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as file:
            file.write(data)
            file.flush()
            os.fchmod(file.fileno(), mode)
            os.fsync(file.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary): os.unlink(temporary)


def ctl(*arguments):
    subprocess.run(['systemctl', '--user', *arguments], check=True)


def install(settings, binary, no_start, source):
    if not binary.is_file(): raise ValueError('Сначала соберите программу: make build')
    for path in settings.values():
        if not path.is_absolute(): raise ValueError('Пути HOME и XDG должны быть абсолютными')
        systemd_quote(path)
    unit_content = render(settings).encode()
    if len(os.fsencode(settings['control'])) >= 108: raise ValueError('Слишком длинный путь управляющего сокета')
    if settings['binary'].is_symlink() or settings['unit'].is_symlink():
        raise ValueError('Установленный бинарь или unit является симлинком; проверьте путь вручную')
    if not no_start:
        if not shutil.which('systemctl'): raise ValueError('systemctl не найден; используйте --no-start для установки без запуска')
        subprocess.run(['systemctl', '--user', 'show-environment'], check=True, stdout=subprocess.DEVNULL)
    # Существующую конфигурацию не перезаписываем даже при обновлении.
    new_config = not settings['config'].exists()
    if not new_config:
        candidate = settings['config']
    elif source:
        candidate = source.resolve()
    else:
        # Начальный сервис только журналирует файлы в отдельном входящем каталоге.
        # Полный демонстрационный пример остаётся единственным example.conf в репозитории.
        settings['inbox'].mkdir(parents=True, exist_ok=True)
        text = f'watch {settings["inbox"]}\nwhen created ** log "Fileward: incoming file"\n'
        candidate = None
    with tempfile.TemporaryDirectory(prefix='fileward-install-') as folder:
        if candidate is None:
            candidate = Path(folder) / 'fileward.conf'
            candidate.write_text(text)
        content = candidate.read_bytes()
        snapshot = Path(folder) / 'validated.conf'
        snapshot.write_bytes(content)
        validation = subprocess.run([str(binary.resolve()), 'validate', '--config', str(snapshot),
                                     '--check-watch'], check=True, text=True, capture_output=True,
                                    cwd=Path.home())
    settings['journal'].parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    settings['control'].parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    if new_config:
        settings['config'].parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        # O_EXCL предотвращает перезапись файла, появившегося во время установки.
        with settings['config'].open('xb') as file:
            os.fchmod(file.fileno(), 0o600)
            file.write(content)
            file.flush()
            os.fsync(file.fileno())
    atomic_write(settings['binary'], binary.read_bytes(), 0o755)
    atomic_write(settings['unit'], unit_content, 0o644)
    print('Установлен бинарь:', settings['binary'])
    print('Создана новая конфигурация:' if new_config else 'Используется существующая конфигурация:',
          settings['config'])
    print(validation.stdout.replace(str(snapshot), str(settings['config'])), end='')
    print('JSON-журнал:', settings['journal'])
    if no_start:
        print('Запуск systemd пропущен (--no-start).')
        return
    ctl('daemon-reload')
    ctl('enable', 'fileward.service')
    # restart обновляет и уже работающую службу, в отличие от enable --now.
    if subprocess.run(['systemctl', '--user', 'is-failed', '--quiet', 'fileward.service']).returncode == 0:
        ctl('reset-failed', 'fileward.service')
    ctl('restart', 'fileward.service')
    subprocess.run([str(settings['binary']), 'health', '--socket', str(settings['control'])], check=True)
    print('Fileward запущен. Логи: journalctl --user -u fileward -f')


def uninstall(settings, no_start):
    if not no_start:
        ctl('stop', 'fileward.service')
        ctl('disable', 'fileward.service')
    for key in ('unit', 'binary'):
        path = settings[key]
        if path.exists() or path.is_symlink(): path.unlink()
    if not no_start: ctl('daemon-reload')
    print('Бинарь и unit удалены. Конфигурация и журналы сохранены.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('install', 'uninstall', 'health', 'reload'))
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/fileward')
    parser.add_argument('--config', type=Path, help='Начальная конфигурация, если установленной ещё нет')
    parser.add_argument('--no-start', action='store_true', help='Не обращаться к пользовательскому systemd')
    args = parser.parse_args()
    settings = paths()
    try:
        if args.command == 'install': install(settings, args.binary, args.no_start, args.config)
        elif args.command == 'uninstall': uninstall(settings, args.no_start)
        else: return subprocess.run([str(settings['binary']), args.command, '--socket', str(settings['control'])]).returncode
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr, end='' if error.stderr.endswith('\n') else '\n', file=sys.stderr)
        print('Ошибка:', error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
