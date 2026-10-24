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

Текущий этап — `v0.5`: сервисный запуск, лог-файл и перезагрузка правил без остановки процесса. Часть возможностей `v0.6` уже есть в коде, но пока не считается завершённым релизом.

Текущий фокус:

- рекурсивное наблюдение через `inotify`;
- правила `log` и `move` из конфигурационного файла;
- foreground и daemon-запуск, `dry-run` и журнал событий;
- user/system `systemd` units и перезагрузка правил через `SIGHUP`.

Ближайшие следующие шаги:

- завершить функции `v0.6` и покрыть поведение сервиса автоматическими проверками;
- расширить условия правил и действия согласно roadmap ниже.

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

- добавить foreground/background режимы запуска;
- добавить pidfile/lock для единственного экземпляра;
- добавить команды `start`, `stop`, `status`, `reload`;
- вести журнал обработанных событий в state-file;
- обрабатывать события через простую очередь в основном цикле;
- сделать запуск более похожим на настоящий local automation daemon.

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
./build/fileward start --daemon --config example.conf --pidfile /tmp/fileward.pid --state-file /tmp/fileward.state ~/Downloads
```

Проверка состояния:

```bash
./build/fileward status --pidfile /tmp/fileward.pid
```

Остановка:

```bash
./build/fileward stop --pidfile /tmp/fileward.pid
```

В этом этапе конфигурация поддерживает:
- `watch <path>` — каталог для наблюдения
- `when <event> <pattern> move <target>` — перемещение совпадающих файлов
- `when <event> <pattern> log "message"` — логирование события
- glob-паттерны `**/*.txt` для вложенных директорий
- путь внутри watch-дерева, например `docs/**/*.md`

## Лицензия

MIT. См. `LICENCE`.
