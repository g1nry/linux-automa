# Fileward и Wazuh

Проверяемая цепочка: `events.jsonl → wazuh-logcollector → JSON decoder → правила
manager → alerts.json → индексатор → Dashboard`. Проверка `wazuh-logtest` относится
только к декодеру и правилам. Она не подтверждает чтение файла, создание настоящего
alert или индексацию.

Комплект проверен с **Wazuh manager 4.14.0**. Для других версий повторите проверки
ниже с их штатным ruleset. Схема Fileward остаётся `schema_version=1`.

## 1. Найти работающий Fileward

Если уже установлен `fileward.service`, используйте его. Второй экземпляр над тем
же деревом может выполнить действия повторно. В оболочке пользователя Fileward:

```sh
config="${XDG_CONFIG_HOME:-$HOME/.config}/fileward/fileward.conf"
journal="${XDG_STATE_HOME:-$HOME/.local/state}/fileward/events.jsonl"
systemctl --user cat fileward.service
grep -n '^[[:space:]]*watch ' "$config"
"$HOME/.local/bin/fileward" validate --config "$config" --check-watch
make health-user
```

При нестандартном unit возьмите фактические `--config` и `--journal` из ExecStart.
Редактирование `fileward.conf` в репозитории не изменяет установленную копию.
После изменения правил примените `make reload-user`; изменение watch требует
рестарта. Каталог из `watch` должен уже существовать, с точным регистром имени.

## 2. Сначала подготовить правила manager

Проверьте, что ID `100800`–`100805` свободны:

```sh
sudo grep -R -nE 'id="10080[0-5]"' /var/ossec/etc/rules /var/ossec/ruleset/rules
```

Обновите существующий файл Fileward, если он уже установлен. Не держите старую
копию этих же правил в `local_rules.xml` или другом файле. Для новой установки
перенесите **один** поставляемый `local_rules.xml` в
`/var/ossec/etc/rules/fileward_rules.xml`, сохранив принятые на manager владельца
и права файлов правил. Родитель 100800 находится перед дочерними правилами.

В Wazuh 4.14.0 штатное правило
[86600](https://github.com/wazuh/wazuh/blob/v4.14.0/ruleset/rules/0475-suricata_rules.xml)
определяет Suricata по непустым `timestamp` и `event_type`. Такие поля есть у
полных событий Fileward. Поэтому правила 100801–100805 принимают **любого**
родителя из `<if_sid>100800,86600</if_sid>` и каждое повторяет ограничения
`provider=fileward`, `schema_version=1`. Короткий JSON без этих двух полей не
воспроизводит исходную проблему. Базовое 100800 также необходимо: через него
проходят записи с пустым `event_type`, включая ошибки запуска.

| ID | Уровень | Условие |
| --- | --- | --- |
| 100800 | 0 | Базовая запись Fileward поддерживаемой схемы |
| 100801 | 5 | `action_result`, `outcome=failure` |
| 100802 | 7 | `event_dropped`: потеря очереди или наблюдения |
| 100803 | 5 | `event_cancelled`, `outcome=failure`: готовность файла |
| 100804 | 5 | Отказ конфигурации, reload, watcher, runtime или state |
| 100805 | 3 | Необязательный лабораторный alert успешного действия |

По умолчанию 100805 **закомментировано**. Для лаборатории раскомментируйте только
этот блок **в том же файле**. Он создаёт alert на каждое успешное действие: у одного
файла могут быть `log`, `copy`, `archive` и `move`. При массовом импорте объём велик.
Для выключения снова закомментируйте блок и перезапустите manager.

Не выносите лабораторное правило в файл, который сортируется раньше родителя.
Wazuh читает `rule_dir` [по алфавиту](https://documentation.wazuh.com/current/user-manual/reference/ossec-conf/ruleset.html#rule-dir);
неизвестные родители дают предупреждения 7617/7619 и пропуск правила.

Итоговые alerts 100801–100805 на проверенном ruleset имеют только группу `fileward`:
`ids`/`suricata` не наследуются из 86600. При выключенном 100805 успешные полные
события с непустым `event_type` могут остаться на 86600 уровня 0 с группами
`ids,suricata`; это не alerts при штатном пороге 3. Записи с пустым `event_type`
остаются на 100800. Fileward не переопределяет штатные правила Suricata.

## 3. Подключить единственный JSONL-источник

На узле Fileward добавьте [agent-localfile.xml](agent-localfile.xml) внутрь
`<ossec_config>` файла `/var/ossec/etc/ossec.conf`, подставив абсолютный путь
к фактическому JSONL. Если Fileward находится на самом manager, localfile
настраивается там; иначе — на агенте конечного узла. Встроенного JSON-декодера
достаточно, декодер текстовых сообщений Fileward не нужен.

При переходе со старой интеграции найдите прежние источники journald,
`/var/log/fileward/fileward.log` и старые текстовые правила вроде 100150.
Уберите только устаревший сбор Fileward после сохранения резервной копии конфига;
другие источники journald могут быть нужны. Не собирайте одно событие одновременно
из текста и JSONL. Старые alerts остаются в истории после изменения конфигурации.

Новый журнал создаётся с правами **0600**, его каталоги — обычно 0700.
Проверьте фактическую учётную запись коллектора и каждый компонент пути:

```sh
ps -eo user,group,args | grep '[w]azuh-logcollector'
namei -l "$journal"
stat -c '%a %U:%G %n' "$journal"
sudo head -n 1 "$journal"
```

Последняя команда проверяет чтение от root. Если коллектор работает от другого
пользователя, выполните проверку именно от его имени через `sudo -u ИМЯ head ...`;
доступ от владельца Fileward ничего не доказывает для коллектора. Также проверьте
ограничения unit, SELinux/AppArmor и проход по родительским каталогам.

Не раскрывайте домашний каталог через `chmod -R`. Разовый chmod/ACL текущего файла
не решает ротацию: Fileward переименовывает inode и создаёт новый JSONL с режимом
0600; даже default ACL ограничивается этим режимом. При коллекторе без необходимых
прав нужен отдельно спроектированный экспорт в защищённый каталог. В этой версии
режим журнала не ослабляется автоматически.

Подключайте только текущий `events.jsonl`, без маски `.1`–`.3`, чтобы не собирать
те же строки повторно. Проверьте чтение после настоящей ротации на своём узле и
подберите скорость сбора/хранение: удаление непрочитанного сегмента теряет события.
У Wazuh 4.14.0 параметр `logcollector.vcheck_files` по умолчанию равен 64 секундам;
смена inode может быть замечена с задержкой. При диагностике дождитесь проверки,
а для интенсивного потока согласуйте интервал с размером сегментов. Описание
[параметров коллектора](https://documentation.wazuh.com/current/user-manual/reference/internal-options.html#logcollector).

## 4. Проверить загрузку и полные записи

На manager, до перезапуска:

```sh
sudo /var/ossec/bin/wazuh-analysisd -t
sudo /var/ossec/bin/wazuh-logcollector -t
```

На отдельном агенте выполните также `wazuh-logcollector -t` там. Успешного кода
недостаточно: не должно быть предупреждений о неизвестных родителях и дубликатах ID.
Затем перезапустите manager и, при изменении localfile на отдельном узле, его агент:

```sh
sudo systemctl restart wazuh-manager
# Только на отдельном узле с агентом:
sudo systemctl restart wazuh-agent
```

Из копии репозитория на manager проверьте комплект полных записей:

```sh
sudo python3 tests/integration/wazuh-logtest.py
# Вместо предыдущей команды, если включили 100805:
sudo python3 tests/integration/wazuh-logtest.py --lab
```

Проверяются ID, уровни и полный набор групп для успеха, отказов, потери событий,
готовности, запуска с пустым `event_type`, base64, Suricata и чужого провайдера.
Описание и просмотр исходных строк: [fixtures](../../tests/fixtures/wazuh/README.md).
Реальную строку своего журнала также передайте в `sudo /var/ossec/bin/wazuh-logtest`.

## 5. Довести новое событие до Dashboard

Включите лабораторное 100805 и после перезапуска компонентов создайте **новый**
файл для реально работающих правил. При использовании единственного `example.conf`
из репозитория все пути согласованы с `~/Downloads/FilewardLab`:

```sh
lab="$HOME/Downloads/FilewardLab"
mkdir -p "$lab/staging" "$lab/inbox"
probe="important-wazuh-$(date +%s)-$$.pdf"
truncate -s 2048 "$lab/staging/$probe"
mv -- "$lab/staging/$probe" "$lab/inbox/$probe"
```

Этот блок рассчитан именно на `watch ~/Downloads/FilewardLab/inbox` из example.conf.
Если установленный watch отличается, задайте `lab` и inbox по нему, проверьте конфиг
и не запускайте второй процесс. Staging должен быть вне watch, на той же файловой
системе, что inbox. Тогда `mv` создаёт нужное `moved_to`; создание прямо в inbox
даёт `created`, и правила `when moved_to` не срабатывают. PDF здесь — заглушка:
проверяются имя, размер и возраст, а не формат содержимого.

Сначала найдите в JSONL `action_result` этого имени. На manager задайте `probe`
тем же точным именем, а `source` — путём JSONL **на конечном узле**:

```sh
source=/home/USER/.local/state/fileward/events.jsonl
sudo jq -c --arg probe "$probe" --arg source "$source" '
  select(.rule.id == "100805" and .location == $source
    and .data.provider == "fileward" and .data.schema_version == "1"
    and .data.kind == "action_result" and .data.outcome == "success"
    and .data.relative_path == $probe)
' /var/ossec/logs/alerts/alerts.json
```

JSON-декодер Wazuh 4.14.0 помещает числовую схему в `data.schema_version` как строку.
Проверьте также `agent.id`, время нового события и путь `data.destination`.
В Dashboard выберите узкий интервал времени и фильтры `rule.id:100805`,
`data.provider:fileward`, точное имя `data.relative_path` и нужный агент.
Если alert есть в `alerts.json`, а в Dashboard отсутствует, дальше проверяйте
Filebeat/индексатор и диапазон времени. `grep fileward` захватывает старые alerts
и команды sudo с этим словом и не доказывает доставку нового события.

## Диагностика по звеньям

```sh
journalctl --user -u fileward.service -n 50 --no-pager
sudo tail -n 100 /var/ossec/logs/ossec.log
sudo cat /var/ossec/var/run/wazuh-logcollector.state
```

Отсутствует запись в JSONL — проверяйте установленный watch, тип события, фильтры
и `health`. Есть JSONL, нет чтения — проверяйте localfile, права и сообщения
logcollector. Счётчики источника в `wazuh-logcollector.state` помогают отличить
чтение от применения правил; [по умолчанию они обновляются раз в 60 секунд](https://documentation.wazuh.com/current/user-manual/reference/statistics-files/wazuh-logcollector-state.html).
Есть чтение, нет alert — проверьте полную строку logtest, ID и уровень; успех без
100805 не обязан создавать alert. Для Shuffle нужен уже созданный alert группы
`fileward`, как описано в [общем руководстве](../README.md#shuffle).
