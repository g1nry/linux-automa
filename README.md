# Fileward

Fileward — лёгкий локальный демон автоматизации для Linux.

Он следит за событиями файловой системы, проверяет их по правилам и выполняет действия `log` и `move`. Копирование, архивирование и запуск команд запланированы на `v0.8`.

Проект начинается как небольшой watcher на базе `inotify`, но дальше должен вырасти в локальный event-driven automation daemon.

## Структура проекта

- `src/` — исходные файлы C
- `include/fileward/` — заголовочные файлы
- `systemd/` — user-service и шаблон system-service
- `tests/` — Linux smoke-тесты сервиса и демона
- `.github/workflows/ci.yml` — сборка и проверки в GitHub Actions
- `meson.build`, `Makefile`, `README.md` — корневой проектный каркас

## Идея

```text
Triggers -> Rules -> Actions
```

То есть:

```text
Событие произошло -> правило совпало -> действие выполнено
```

Примеры:

```text
PDF появился в ~/Downloads       -> переместить в ~/Documents/PDF
ZIP появился в ~/Downloads       -> переместить в ~/Archives
TXT появился в ~/Downloads       -> записать сообщение в лог
```

## Текущий статус

Текущий этап — `v0.6`: daemon core с командами управления, блокировкой pidfile, журналом действий и очередью событий.

Текущий фокус:

- рекурсивное наблюдение через `inotify`;
- правила `log` и `move` из конфигурационного файла;
- foreground и daemon-запуск, `dry-run` и журнал обработанных действий;
- `start`, `stop`, `status`, `reload` и защита от двух процессов с одним pidfile;
- обработка событий через очередь в основном цикле;
- user/system `systemd` units и перезагрузка правил через `SIGHUP`.

Ближайшие следующие шаги:

- расширить условия правил в `v0.7`;
- добавить новые действия в `v0.8` согласно roadmap ниже.

## Сборка

Нужны:

- Linux (`inotify`); для service-режима — systemd;
- C-компилятор;
- Meson;
- Ninja и Make.

На Ubuntu/Debian:

```bash
sudo apt-get update
sudo apt-get install -y build-essential meson ninja-build
```

Сборка:

```bash
meson setup build
meson compile -C build
```

Подготовка примера и справка (команды выполняются из корня репозитория):

```bash
mkdir -p ~/Downloads
test -e fileward.conf || cp example.conf fileward.conf
./build/fileward help
```

Пример конфига перемещает новые PDF-файлы. Сначала можно посмотреть предполагаемые действия через `--dry-run`. Без конфига программа только выводит события. Для остановки обычного запуска нажмите `Ctrl+C`.

Запуск:

```bash
./build/fileward run ~/Downloads
```

или через `run` target:

```bash
make run
```

Запуск с конфигурационным файлом:

```bash
./build/fileward run --config fileward.conf
```

Запуск в режиме dry-run:

```bash
./build/fileward run --dry-run --config fileward.conf
```

или без конфига:

```bash
./build/fileward run --dry-run ~/Downloads
```

Проверка пути и правил (файл должен существовать внутри `watch`; действия не выполняются):

```bash
./build/fileward test --config fileward.conf ~/Downloads/report.pdf
```

Объяснение совпадений:

```bash
./build/fileward explain --config fileward.conf --event created ~/Downloads/report.pdf
```

Перезагрузка конфигурации через SIGHUP:

```bash
kill -HUP 12345
```

Замените `12345` на PID своего процесса. Для фонового запуска удобнее команда `reload`, для systemd — `systemctl reload` (примеры ниже).

Если `fileward` запущен с `--config fileward.conf`, он перечитает конфиг и применит новые правила без перезапуска.
При ошибке в новом конфиге старые правила продолжают работать. Если поменялся каталог `watch`, нужен перезапуск сервиса.

Запись логов в файл при обычном или фоновом запуске:

```bash
./build/fileward run --config fileward.conf --log-file ~/fileward.log
```

Каталог для наблюдения можно передать после опций: он заменит `watch` из конфига, в том числе при reload. Относительные пути считаются от рабочей папки запуска, которую демон сохраняет. Для systemd-конфигов используйте абсолютные пути или `~/...`. Родительские каталоги для log/state/pid-файлов должны существовать; храните эти файлы вне наблюдаемого дерева.

Основные команды:

| Команда | Назначение |
| --- | --- |
| `help`, `--help` | Справка |
| `run` | Наблюдение в foreground; `run --daemon` включает фоновый запуск |
| `start` | Запуск в фоне с подтверждением запуска watcher-а |
| `stop` | Отправка SIGTERM и ожидание остановки до 5 секунд |
| `status` | Печать `running` или `stopped` по pidfile |
| `reload` | Отправка SIGHUP; результат перечитывания виден в логе |
| `test` | Проверка существующего пути и правил для события `created` |
| `explain` | Проверка существующего пути для выбранного `--event` |

`run/start`: `--config`, `--dry-run`, `--pidfile`, `--log-file`, `--state-file`; `run` также принимает `--daemon`. `stop/status/reload` принимают `--pidfile`. `test/explain` не запускают watcher и не изменяют файлы.

Коды завершения: `0` — успешное выполнение, `1` — ошибка аргументов/пути вне `watch`, `2` — ошибка конфига, `3` — ошибка выполнения. `status` возвращает `0` и для `stopped`: проверяйте его текстовый вывод.

## Makefile workflow

Для повседневной разработки удобнее использовать `Makefile`. `make help` показывает все цели; `make clean` удаляет каталог сборки.

Собрать проект:

```bash
make build
```

Быстро проверить сборку и базовое поведение CLI:

```bash
make check
```

`make check` собирает проект и проверяет, что `fileward help` работает и выводит подсказку.

Проверки reload, log-file, блокировки pidfile, start/stop и state-file:

```bash
bash tests/service-smoke.sh
bash tests/daemon-smoke.sh
```

Эти проверки также запускаются в GitHub Actions на Ubuntu при push и pull request. Они проверяют сам процесс; установку и запуск unit-файлов под systemd нужно проверять отдельно.

Установить бинарь и user-service:

```bash
make install-user
```

Перезапустить user-service после изменений:

```bash
make restart-user
```

Посмотреть статус:

```bash
make status-user
```

Смотреть логи:

```bash
make logs-user
```

Удалить user-service и установленный бинарь:

```bash
make uninstall-user
```

`install-user` ставит бинарь в `~/.local/bin/fileward`, service-файл в `~/.config/systemd/user/fileward.service`, создаёт `~/.config/fileward/fileward.conf` с `watch ~/Downloads` (если файла ещё нет) и запускает сервис через `systemctl --user`. Логи сервиса доступны через `make logs-user`.

Запускайте `make install-user` без `sudo`. Начальный конфиг не содержит действий — добавьте свои правила. Журнал успешных действий хранится в `~/.local/share/fileward.state`; `make uninstall-user` оставляет конфиг и журнал. После обновления уже установленного сервиса используйте `make restart-user`.

Остановить и снова запустить user-service:

```bash
systemctl --user stop fileward.service
systemctl --user start fileward.service
```

У systemd-сервисов нет CLI pidfile: управляйте ими через `systemctl`, а состояние смотрите через `make status-user`.

После изменения правил:

```bash
systemctl --user reload fileward.service
```

Для system-service под конкретным пользователем есть шаблон `systemd/system/fileward@.service`. Перед переходом с user-service остановите его: `systemctl --user disable --now fileward.service`, чтобы один каталог не обрабатывался двумя процессами. Пример установки для текущего пользователя:

```bash
mkdir -p ~/Downloads
meson configure build --prefix=/usr/local --bindir=bin
sudo meson install -C build
sudo install -m 644 systemd/system/fileward@.service /etc/systemd/system/
sudo install -d -m 755 /etc/fileward
if ! sudo test -e "/etc/fileward/$USER.conf"; then
  printf 'watch %s/Downloads\n' "$HOME" | sudo tee "/etc/fileward/$USER.conf" >/dev/null
fi
sudo systemctl daemon-reload
sudo systemctl enable --now "fileward@$USER.service"
```

System-service запускает процесс от имени выбранного пользователя и читает `/etc/fileward/<user>.conf`.

Управление system-service:

```bash
sudo systemctl status "fileward@$USER.service" --no-pager
sudo systemctl reload "fileward@$USER.service"
sudo systemctl restart "fileward@$USER.service"
sudo journalctl -u "fileward@$USER.service" -f
sudo systemctl disable --now "fileward@$USER.service"
```

Шаблон ожидает бинарь `/usr/local/bin/fileward`. В отличие от user-service он не задаёт `--state-file`; обычные логи пишет в journal.

## Быстрая проверка без Meson

Для быстрой проверки компиляции:

```bash
cc -std=c11 -Wall -Wextra -Isrc -Iinclude src/main.c src/watcher.c src/log.c src/config.c src/action.c src/glob.c -o /tmp/fileward
```

Запуск:

```bash
/tmp/fileward ~/Downloads
```

## Пример конфигурации

```text
watch ~/Downloads

when created *.pdf move ~/Documents/PDF
when created *.txt log "text file created"
```

Самостоятельный пример проверки в отдельном временном каталоге:

```bash
demo_dir=$(mktemp -d)
mkdir "$demo_dir/watch"
printf 'watch %s/watch\nwhen created *.txt log "example"\n' "$demo_dir" > "$demo_dir/fileward.conf"
touch "$demo_dir/watch/report.txt"
./build/fileward test --config "$demo_dir/fileward.conf" "$demo_dir/watch/report.txt"
./build/fileward explain --config "$demo_dir/fileward.conf" --event created "$demo_dir/watch/report.txt"
```

Быстрый запуск в фоне:

```bash
mkdir -p ~/Downloads "$HOME/.local/state/fileward"
./build/fileward start --config example.conf \
  --pidfile "$HOME/.local/state/fileward/fileward.pid" \
  --log-file "$HOME/.local/state/fileward/fileward.log" \
  --state-file "$HOME/.local/state/fileward/fileward.state"
```

Проверка состояния:

```bash
./build/fileward status --pidfile "$HOME/.local/state/fileward/fileward.pid"
```

Перезагрузка правил:

```bash
./build/fileward reload --pidfile "$HOME/.local/state/fileward/fileward.pid"
```

Просмотр логов и журнала действий:

```bash
tail -f "$HOME/.local/state/fileward/fileward.log"
tail -n 20 "$HOME/.local/state/fileward/fileward.state"
```

`Ctrl+C` завершает просмотр `tail -f`. Остановить сам демон:

```bash
./build/fileward stop --pidfile "$HOME/.local/state/fileward/fileward.pid"
```

Во всех командах управления указывайте один и тот же pidfile. По умолчанию используется `/tmp/fileward.pid`; явный путь в пользовательском каталоге позволяет разделять экземпляры. Pidfile остаётся после остановки: блокировка файла показывает, работает ли процесс. Не удаляйте его, пока процесс работает. Повторный `start` с тем же pidfile завершится с ошибкой, пока первый экземпляр активен.

State-file создаётся при первом успешном действии и содержит строки `время<TAB>событие<TAB>watch<TAB>относительный путь<TAB>действие`. `dry-run`, действия с ошибкой и события без совпавшего правила не добавляют записи. Это журнал, а не очередь для восстановления после перезапуска. Без `--log-file` фоновый процесс отправляет вывод в `/dev/null`.

В этом этапе конфигурация поддерживает:

- `watch <path>` — каталог для наблюдения
- `when <event> <pattern> move <target>` — перемещение совпадающих файлов
- `when <event> <pattern> log "message"` — логирование события
- glob-паттерны `**/*.txt` для вложенных директорий
- путь внутри watch-дерева, например `docs/**/*.md`

События: `created`, `modified`, `deleted`, `moved_from`, `moved_to`. Шаблон сравнивается с путём относительно `watch`: `*.txt` — файлы в корне, `**/*.txt` — также во вложенных каталогах. Поддерживается один `watch`; несколько совпавших правил выполняются в порядке записи. Строки, начинающиеся с `#`, считаются комментариями. Путь назначения `move` не поддерживает пробелы.

## Ограничения v0.6

- `move` работает через переименование в пределах одной файловой системы и может заменить существующий одноимённый файл в каталоге назначения. Безопасное разрешение конфликтов ещё в roadmap.
- `created` означает появление файла, но не завершение его записи. Автоматического ожидания окончания загрузки пока нет.
- На старте добавляются watches для существующих каталогов, но правила для уже лежащих файлов не запускаются. Автоматического восстановления пропущенных событий нет.
- Лимит — 512 наблюдаемых каталогов; события при переполнении очереди ядра `inotify` не восстанавливаются. Перемещения каталогов пока не полностью обрабатывают изменение путей вложенных watches.

## Roadmap

### v0.6 — daemon core

- foreground/background режимы запуска;
- pidfile с блокировкой для единственного экземпляра;
- команды `start`, `stop`, `status`, `reload`;
- журнал успешно обработанных действий в state-file;
- очередь событий с обработкой в основном цикле;
- подтверждение успешного старта демона и ожидание остановки в `stop`.

### v0.7 — richer rules

- добавить условия по размеру, времени изменения, имени и расширению;
- расширить управление несколькими правилами на одно событие (сейчас совпавшие правила выполняются по порядку);
- поддержать приоритеты правил и порядок выполнения;
- добавить поддержку `ignore` и `allow`/`deny` списков.

### v0.8 — richer actions

- добавить действия `copy`, `archive`, `exec`;
- добавить безопасную обработку конфликтов файлов;
- добавить retry/backoff для временных ошибок;
- добавить поддержку шаблонов для путей назначения.

### v0.9 — observability

- сделать структурированные логи;
- добавить метрики: число событий, обработанных правил, ошибок;
- добавить историю выполненных действий;
- добавить команду `history`/`stats`.

### v1.0 — local automation engine

- сделать архитектуру расширяемой: watcher, dispatcher, executor, state;
- поддержать plugin-like или modular action handlers;
- сделать проект удобным для повседневного использования как локальный automation daemon;
- обеспечить стабильный запуск через `systemd`, с reload, health-check и понятной документацией.

## Лицензия

MIT. См. `LICENCE`.
