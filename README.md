# Fileward

Fileward — лёгкий локальный демон автоматизации для Linux.

Он следит за событиями файловой системы, проверяет их по простым правилам и выполняет действия: пишет события в журнал, перемещает файлы, копирует их или запускает команды.

Проект начинается как небольшой watcher на базе `inotify`, но дальше должен вырасти в локальный event-driven automation daemon.

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

Проект находится на раннем этапе разработки и пока движется к `v0.1`.

Текущий фокус:

- запуск из терминала;
- наблюдение за одной директорией через `inotify`;
- вывод событий создания, изменения, удаления и перемещения файлов;
- корректная остановка по `Ctrl+C`.

Ближайшие следующие шаги:

- поддержка конфигурационного файла;
- простые правила;
- действия `log` и `move`;
- режим `dry-run`;
- user/system `systemd` service-файлы.

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
./build/fileward ~/Downloads
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

`install-user` ставит бинарь в `~/.local/bin/fileward`, service-файл в `~/.config/systemd/user/fileward.service` и запускает сервис через `systemctl --user`.

## Быстрая проверка без Meson

Для быстрой проверки компиляции:

```bash
cc -std=c11 -Wall -Wextra -Isrc src/main.c src/watcher.c src/log.c -o /tmp/fileward
```

Запуск:

```bash
/tmp/fileward ~/Downloads
```

## Roadmap

### v0.1 — базовый watcher

- Принимать путь к директории аргументом.
- Следить за директорией через `inotify`.
- Печатать файловые события.
- Обрабатывать `SIGINT` и `SIGTERM`.

### v0.2 — конфигурация

- Добавить `--config`.
- Реализовать простой формат конфига.
- Поддержать директиву `watch`.
- Поддержать простые правила `when`.

### v0.3 — действия

- Добавить действие `log`.
- Добавить действие `move`.
- Добавить режим `dry-run`.

### v0.4 — CLI

- Добавить `fileward test <path>`.
- Добавить explain-style вывод.
- Улучшить help и коды возврата.

### v0.5 — сервисный режим

- Добавить `systemd` unit-файлы.
- Добавить поддержку лог-файла.
- Добавить перезагрузку конфига через `SIGHUP`.

## Пример будущего конфига

```text
watch ~/Downloads

when created *.pdf move ~/Documents/PDF
when created *.png move ~/Pictures/Screenshots
when created *.zip move ~/Archives
when modified *.c run "make test"
```

## Лицензия

MIT. См. `LICENCE`.
