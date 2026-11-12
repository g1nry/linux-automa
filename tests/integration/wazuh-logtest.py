#!/usr/bin/env python3
"""Проверка полных записей настоящим wazuh-logtest; конфигурацию не изменяет."""
import argparse
import ast
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--logtest', default='/var/ossec/bin/wazuh-logtest')
parser.add_argument('--lab', action='store_true', help='Ожидать включённое правило успеха 100805')
parser.add_argument('--case', help='Проверить только названный пример')
parser.add_argument('--print-record', action='store_true', help='Вывести исходные JSON без запуска Wazuh')
args = parser.parse_args()
fixtures = Path(__file__).resolve().parents[1] / 'fixtures/wazuh/cases.json'
cases = json.loads(fixtures.read_text())
if args.case:
    cases = [case for case in cases if case['name'] == args.case]
    if not cases:
        parser.error('Неизвестное имя примера')
for case in cases:
    line = json.dumps(case['record'], ensure_ascii=False) + '\n'
    if args.print_record:
        print(line, end='')
        continue
    expected = case['lab' if args.lab else 'default']
    result = subprocess.run([args.logtest, '-U', f"{expected['id']}:{expected['level']}:json"],
                            input=line, text=True, capture_output=True, timeout=30)
    output = result.stdout + result.stderr
    # Сам код завершения не доказывает, что logtest смог подключиться к analysisd.
    rule = re.search(r"\bid: '(\d+)'", output)
    level = re.search(r"\blevel: '(\d+)'", output)
    groups = re.search(r"\bgroups: '(\[.*\])'", output)
    assert result.returncode == 0 and rule and level and groups, (case['name'], output)
    assert (int(rule[1]), int(level[1])) == (expected['id'], expected['level']), (case['name'], output)
    assert sorted(ast.literal_eval(groups[1])) == sorted(expected['groups']), (case['name'], output)
    assert not re.search(r'WARNING|ERROR|CRITICAL', output), (case['name'], output)
    print(f"{case['name']}: {rule[1]}, level {level[1]}, groups {groups[1]}")
