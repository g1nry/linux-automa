# Установка и эксплуатация v1.0

## Быстрый запуск

На Debian/Ubuntu из корня репозитория:

```sh
sudo apt-get install -y build-essential meson ninja-build pkg-config zlib1g-dev python3
make install-user
make health-user
```

Пользовательский systemd должен быть доступен в текущей сессии:
`systemctl --user is-system-running`. Установка сервиса выполняется без sudo.
Установщик Makefile использует оптимизированную сборку `build-release/`;
`make build` сохраняет отдельный отладочный профиль. Сборка требует Linux и glibc >= 2.34; libsystemd для неё не нужен.
При первой установке создаётся каталог `~/Downloads/Fileward/inbox` и одно
правило `log`: исходные файлы остаются на месте. Попробуйте
`touch ~/Downloads/Fileward/inbox/hello.txt`, затем `make logs-user`.

Для начальной установки своего конфига используйте
`make install-user CONFIG=fileward.conf`. Если установленный конфиг уже есть,
он сохраняется, а `CONFIG` не применяется. Единственный общий пример в репозитории —
`example.conf`; установщик не создаёт дополнительных демонстрационных конфигов в дереве проекта.

## Где находятся файлы

| Объект | Путь по умолчанию |
| --- | --- |
| Исполняемый файл | `~/.local/bin/fileward` |
| Конфигурация | `~/.config/fileward/fileward.conf` |
| Unit | `~/.config/systemd/user/fileward.service` |
| Аудит | `~/.local/state/fileward/events.jsonl` и сегменты `.1`–`.3` |
| Управляющий сокет | `$XDG_RUNTIME_DIR/fileward/control.sock` |
| Логи для человека | `journalctl --user -u fileward -f` |

Учитываются `XDG_CONFIG_HOME` и `XDG_STATE_HOME`. Если XDG_RUNTIME_DIR не задан,
используется доступный `/run/user/<uid>`; иначе сокет размещается в каталоге
состояния. Все пути должны быть абсолютными. Ограничение пути Unix-сокета —
107 байт; для длинных XDG-путей выберите короткий runtime-каталог.
Данные, сокет и журналы должны находиться вне `watch`.

Служба работает из домашнего каталога пользователя. Используйте абсолютные пути
или `~/...` в правилах. Установщик проверяет синтаксис до замены бинаря и unit,
но доступность watch проверяется при запуске. Ошибку запуска смотрите через
`make status-user` и `make logs-user`.

## Управление и обновление

```sh
make health-user
make reload-user
make status-user
make logs-user
make restart-user
```

`restart-user` повторяет сборку и установку, атомарно обновляет бинарь и unit,
перечитывает units и перезапускает службу. Конфигурация и JSONL сохраняются. Новый unit использует JSONL как основной
журнал и не включает прежний необязательный `--state-file`: существующий
`~/.local/share/fileward.state` сохраняется, но больше не пополняется службой.
Сам параметр TSV поддерживается для ручного запуска и собственных units.
После изменения правил достаточно `make reload-user` или
`systemctl --user reload fileward`. Успех означает, что новый снимок опубликован;
принятые ранее задания заканчивают работу по прежним правилам. Некорректный конфиг
возвращает отказ и сохраняет прежнее поколение. Изменение watch требует рестарта.

`make uninstall-user` останавливает и отключает службу, удаляет бинарь и unit,
сохраняя конфиг и журналы. Остановить без удаления:
`systemctl --user stop fileward`; запустить снова: `systemctl --user start fileward`.
Для работы user-service после выхода из учётной записи администратор может включить
linger: `sudo loginctl enable-linger "$USER"`.

Для подготовки файлов без доступного manager:

```sh
make build
python3 scripts/manage-user.py install --no-start
```

Такая установка не включает и не запускает службу. После входа в сессию можно
повторить обычный `make install-user`. Unit генерируется из
`systemd/user/fileward.service.in`, учитывает пути с пробелами и спецсимволами.

## Readiness и восстановление

User/system units используют `Type=notify`, проверку конфига до запуска и
health после READY. Уведомление готовности отправляется после запуска наблюдателя,
рабочих потоков, аудита и управляющего сокета. Watchdog получает heartbeat
из главного цикла каждые 10 секунд при штатном интервале 20 секунд.
Отказ процесса или зависание главного цикла вызывает рестарт с задержкой 2 секунды.
Пять запусков за 60 секунд ограничивают бесконечные рестарты ошибочной конфигурации;
после исправления используйте `systemctl --user reset-failed fileward` и запуск.

При остановке отменяются ожидающие задания и активные команды. Через 10 секунд
systemd принудительно завершает всю группу службы, если процесс не остановился.
`NoNewPrivileges=true` запрещает действиям получать новые привилегии через setuid.

Рестарт восстанавливает процесс и подписки. Незавершённые задания не сохраняются,
уже лежащие файлы не переигрываются, отката операций и гарантии exactly-once нет.
После SIGKILL могут остаться временные `.fileward-*`; незавершённый хвост JSONL
требует ручного разбора, запуск его не обрезает. Сохраните журнал для анализа,
а затем задайте новый путь либо исправьте хвост осознанно.

## Живой health и протокол управления

```sh
~/.local/bin/fileward health
~/.local/bin/fileward reload
./build/fileward run --config fileward.conf --control-socket /tmp/fileward-demo.sock
./build/fileward health --socket /tmp/fileward-demo.sock
```

`health` обращается к процессу, а `stats` анализирует сохранённый JSONL.
Для процесса с явным `--journal /path/events.jsonl` сокет по умолчанию —
`/path/events.jsonl.sock`; передайте его через `--socket` или задайте
`--control-socket` при запуске. Сервисы задают путь явно.
У нескольких экземпляров должны различаться журналы и сокеты.

Коды health: 0 — healthy, 4 — degraded, 3 — недоступен или ответ не получен за
3 секунды, 1 — неверные аргументы. Схема ответа: [health.schema.json](health.schema.json).

| Поле | Значение |
| --- | --- |
| `schema_version`, `provider`, `program_version` | Версия контракта, источник и версия программы |
| `status`, `code` | `healthy`/`FW_OK` либо `degraded`/`FW_HEALTH_DEGRADED` |
| `pid`, `uptime_seconds` | Процесс и время его работы |
| `audit_healthy`, `watch_coverage_ok`, `watched_directories` | Аудит и полнота подписок |
| `workers`, `queued`, `active`, `capacity` | Размер пула, ожидающие, активные и общий предел |
| `config_generation` | Поколение текущей конфигурации |
| `events_observed`, `events_dropped` | Наблюдения и зафиксированные потери |
| `chains_completed`, `action_failures` | Завершённые цепочки и ошибки действий без отмен при остановке |

Потери из-за заполнения очереди, переполнение inotify или уменьшение покрытия
наблюдения сохраняют degraded до рестарта. Один сигнал переполнения inotify
считается одной зафиксированной потерей: точное число пропущенных событий неизвестно.
Успешный reload не скрывает прошлую потерю. Ошибки конкретных действий увеличивают
счётчик, но сами по себе не означают отказ движка. Все счётчики сбрасываются при
рестарте. Health не доказывает успешную доставку в SIEM и не проверяет завершение
каждого рабочего потока. При отказе аудита процесс прекращает работу.

Протокол: Unix stream, одна строка `health\n` или `reload\n`, ответ — одна строка JSON.
Доступны только владелец UID и root, сокет имеет режим 0600. Запрос ограничен 63
байтами, одновременно обслуживаются до 16 клиентов с секундным таймаутом чтения.
Вызов `exec` через этот протокол отсутствует. Обычный файл или активный сокет на
том же пути не заменяется; оставшийся после сбоя неактивный сокет восстанавливается.

Legacy `reload --pidfile /path/fileward.pid` и SIGHUP сохраняются, но не ждут
подтверждения применения. Для сервиса используйте управляющий сокет или systemctl.

## Фоновый запуск без systemd

Для отдельного процесса из дерева проекта можно использовать `start` и pidfile.
Подготовьте входящие каталоги и отдельные пути аудита и управления, чтобы экземпляр
не конфликтовал с установленным сервисом:

```sh
make build
mkdir -p ~/Downloads/FilewardLab/{inbox,staging} "$HOME/.local/state/fileward"
manual_runtime=$(mktemp -d /tmp/fileward-manual.XXXXXX)
./build/fileward start --config example.conf \
  --pidfile "$HOME/.local/state/fileward/manual.pid" \
  --log-file "$HOME/.local/state/fileward/manual.log" \
  --state-file "$HOME/.local/state/fileward/manual.state" \
  --journal "$HOME/.local/state/fileward/manual-events.jsonl" \
  --control-socket "$manual_runtime/control.sock"
```

Выполняйте команды управления ниже в той же оболочке, где задан `manual_runtime`.
Для управления из другой сессии укажите получившийся путь сокета явно.
Для остановки по pidfile переменная не требуется.

```sh
./build/fileward status --pidfile "$HOME/.local/state/fileward/manual.pid"
./build/fileward health --socket "$manual_runtime/control.sock"
./build/fileward reload --socket "$manual_runtime/control.sock"
tail -f "$HOME/.local/state/fileward/manual.log"
./build/fileward stop --pidfile "$HOME/.local/state/fileward/manual.pid"
```

`Ctrl+C` завершает просмотр `tail -f`, а `stop` — сам демон с ожиданием до 5 секунд.
По умолчанию команды pidfile используют `/tmp/fileward.pid`; явный путь разделяет
экземпляры. Pidfile остаётся после остановки, состояние определяет его блокировка.
Не удаляйте файл работающего процесса. Повторный `start` с тем же pidfile отклоняется.
Без `--log-file` отсоединённый процесс направляет stdout/stderr в `/dev/null`.
`reload --pidfile` также поддерживается, но отправляет SIGHUP без подтверждения.

TSV-файл `--state-file` создаётся после первого успешного действия и содержит
`время<TAB>событие<TAB>watch<TAB>относительный путь<TAB>действие`. Dry-run,
пропуски и ошибки действий не добавляют туда успех. Это совместимый журнал,
а не сохранённая очередь для восстановления. Основной структурированный аудит — JSONL.

## Системная служба

Шаблон `systemd/system/fileward@.service` рассчитан на бинарь `/usr/local/bin/fileward`
и `/etc/fileward/<user>.conf`. Процесс работает как пользователь `%i`, manager
создаёт `/run/fileward-<user>` с режимом 0700. Заранее создайте
`/var/log/fileward/<user>` с владельцем сервиса.

User-service и system-service для одного дерева одновременно не запускайте.
Если пользовательская служба уже установлена, перед переходом остановите её:
`systemctl --user disable --now fileward.service`.

Пример системной установки для текущего пользователя из корня репозитория:

```sh
make build
mkdir -p ~/Downloads/Fileward/inbox
meson configure build --prefix=/usr/local --bindir=bin
sudo meson install -C build
sudo install -m 644 systemd/system/fileward@.service /etc/systemd/system/
sudo install -d -m 755 /etc/fileward
sudo install -d -m 700 -o "$USER" "/var/log/fileward/$USER"
if ! sudo test -e "/etc/fileward/$USER.conf"; then
  printf 'watch %s/Downloads/Fileward/inbox\nwhen created ** log "Входящий файл"\n' "$HOME" | sudo tee "/etc/fileward/$USER.conf" >/dev/null
fi
sudo systemctl daemon-reload
sudo systemctl enable --now "fileward@$USER.service"
```

Проверка и управление:

```sh
sudo systemctl status "fileward@$USER.service"
/usr/local/bin/fileward health --socket "/run/fileward-$USER/control.sock"
sudo systemctl reload "fileward@$USER.service"
sudo journalctl -u "fileward@$USER.service" -f
sudo systemctl disable --now "fileward@$USER.service"
```
