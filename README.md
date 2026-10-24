# Fileward

Fileward — лёгкий локальный демон автоматизации для Linux.

Он следит за событиями файловой системы, проверяет их по простым правилам и выполняет действия: пишет события в журнал, перемещает файлы, копирует их или запускает команды.

Проект начинается как небольшой watcher на базе `inotify`, но дальше должен вырасти в локальный event-driven automation daemon.

## Структура проекта

- `src/` — исходные файлы C
- `include/fileward/` — заголовочные файлы
- `systemd/` — юниты для user/systemd-инсталляции
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
C-файл изменился в проекте       -> запустить тесты
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

- C-компилятор;
- Meson;
- Ninja.

Сборка:

```bash
meson setup build
meson compile -C build
```

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

Проверка пути и правил:

```bash
./build/fileward test --config fileward.conf ~/Downloads/report.pdf
```

Объяснение совпадений:

```bash
./build/fileward explain --config fileward.conf --event created docs/report.pdf
```

Перезагрузка конфигурации через SIGHUP:

```bash
kill -HUP <pid>
```

Если `fileward` запущен с `--config fileward.conf`, он перечитает конфиг и применит новые правила без перезапуска.
При ошибке в новом конфиге старые правила продолжают работать. Если поменялся каталог `watch`, нужен перезапуск сервиса.

Запись логов в файл при обычном или фоновом запуске:

```bash
./build/fileward run --config fileward.conf --log-file ~/fileward.log
```

## Makefile workflow

Для повседневной разработки удобнее использовать `Makefile`.

Собрать проект:

```bash
make build
```

Быстро проверить сборку и базовое поведение CLI:

```bash
make check
```

`make check` собирает проект и проверяет, что `fileward help` работает и выводит подсказку.

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

После изменения правил:

```bash
systemctl --user reload fileward.service
```

Для system-service под конкретным пользователем есть шаблон `systemd/system/fileward@.service`. Пример установки для текущего пользователя:

```bash
sudo meson install -C build
sudo install -m 644 systemd/system/fileward@.service /etc/systemd/system/
sudo install -d -m 755 /etc/fileward
printf 'watch %s/Downloads\n' "$HOME" | sudo tee "/etc/fileward/$USER.conf" >/dev/null
sudo systemctl daemon-reload
sudo systemctl enable --now "fileward@$USER.service"
```

System-service запускает процесс от имени выбранного пользователя и читает `/etc/fileward/<user>.conf`. Перед его запуском остановите user-service для того же каталога: `systemctl --user disable --now fileward.service`.

## Быстрая проверка без Meson

Для быстрой проверки компиляции:

```bash
cc -std=c11 -Wall -Wextra -Isrc -Iinclude src/main.c src/watcher.c src/log.c src/config.c src/action.c src/glob.c -o /tmp/fileward
```

Запуск:

```bash
/tmp/fileward ~/Downloads
```

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
- поддержать несколько правил на одно событие;
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

## Пример конфигурации

```text
watch ~/Downloads

when created *.pdf move ~/Documents/PDF
when created *.txt log "text file created"
```

Минимальная проверка в одну секунду:

```bash
./build/fileward test --config example.conf ~/Downloads/report.pdf
./build/fileward explain --config example.conf --event created ~/Downloads/report.pdf
```

Быстрый запуск в фоне:

```bash
./build/fileward start --config example.conf --pidfile /tmp/fileward.pid --log-file /tmp/fileward.log --state-file /tmp/fileward.state
```

Проверка состояния:

```bash
./build/fileward status --pidfile /tmp/fileward.pid
```

Остановка:

```bash
./build/fileward stop --pidfile /tmp/fileward.pid
```

Перезагрузка правил:

```bash
./build/fileward reload --pidfile /tmp/fileward.pid
```

Pidfile остаётся после остановки: блокировка файла показывает, работает ли процесс. Повторный `start` с тем же pidfile завершится с ошибкой, пока первый экземпляр активен. State-file записывает успешно выполненные действия; `dry-run` не добавляет записи.

В этом этапе конфигурация поддерживает:
- `watch <path>` — каталог для наблюдения
- `when <event> <pattern> move <target>` — перемещение совпадающих файлов
- `when <event> <pattern> log "message"` — логирование события
- glob-паттерны `**/*.txt` для вложенных директорий
- путь внутри watch-дерева, например `docs/**/*.md`

## Лицензия

MIT. См. `LICENCE`.
