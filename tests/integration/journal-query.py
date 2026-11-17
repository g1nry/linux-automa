#!/usr/bin/env python3
"""Эталон history/stats, границы блоков и явная область проверки --tail."""
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/fileward').resolve())
rng = random.Random(112)
with tempfile.TemporaryDirectory(prefix='fw-query-') as folder:
    journal = Path(folder) / 'events.jsonl'
    rows = []
    for n in range(1200):
        kind = rng.choice(['action_result', 'rule_evaluated', 'file_observed', 'file_ready', 'event_finished',
                           'event_dropped', 'event_cancelled', 'action_retry', 'exec_status', 'diagnostic', 'event_coalesced'])
        outcome = rng.choice(['success', 'failure', 'skipped', 'cancelled', 'matched', 'error'])
        row = {'nested': {'kind': 'wrong', 'event_id': 'wrong'}, 'schema_version': 1,
               'event_id': str(n), 'parent_id': str(n // 4), 'kind': kind, 'outcome': outcome, 'code': 'FW_OK',
               'message': 'строка\\\n' + 'x' * rng.randrange(120)}
        if n in (0, 100, 800, 1199):
            target = [65535, 65536, 65537, 131072][(0,100,800,1199).index(n)]
            row['message'] += 'x' * (target - len((json.dumps(row, ensure_ascii=False) + '\n').encode()))
        rows.append(row)
    def encode(row): return (json.dumps(row, ensure_ascii=False) + '\n').encode()
    for suffix, chunk in (('.3', rows[:300]), ('.2', rows[300:600]), ('.1', rows[600:900]), ('', rows[900:])):
        Path(str(journal) + suffix).write_bytes(b''.join(map(encode, chunk)))
    with journal.open('ab') as file: file.write(b'{"incomplete":')
    def run(command, *options):
        return subprocess.run([binary, command, '--journal', str(journal), *options], capture_output=True, timeout=6)
    for all_records in (False, True):
        for parent in (None, '290'):
            for limit in (1, 20, 1000):
                eligible = [r for r in rows if (all_records or r['kind']=='action_result')
                            and (parent is None or r['event_id']==parent or r['parent_id']==parent)]
                expected = b''.join(map(encode, eligible[-limit:]))
                flags = ['--limit', str(limit), *(['--all'] if all_records else []),
                         *(['--event-id', parent] if parent is not None else [])]
                for tail in ([], ['--tail']):
                    result = run('history', *flags, *tail)
                    assert result.returncode == 0 and result.stdout == expected, (flags,tail,result.stderr)
    result = run('stats'); assert result.returncode == 0, result.stderr
    stats = json.loads(result.stdout)
    assert stats['records'] == len(rows) and stats['malformed_records'] == 0, stats
    mappings = {'events_observed':'file_observed','events_ready':'file_ready','events_processed':'event_finished',
                'rules_evaluated':'rule_evaluated','events_coalesced':'event_coalesced','events_dropped':'event_dropped',
                'events_cancelled':'event_cancelled','retries':'action_retry'}
    for key, kind in mappings.items(): assert stats[key] == sum(r['kind']==kind for r in rows), key
    for key,outcome in (('actions_succeeded','success'),('actions_skipped','skipped'),('actions_cancelled','cancelled')):
        assert stats[key] == sum(r['kind']=='action_result' and r['outcome']==outcome for r in rows), key
    assert stats['failures'] == sum(r['outcome']=='failure' and r['kind'] not in ('exec_status','diagnostic') for r in rows)
    assert stats['rules_matched'] == sum(r['kind']=='rule_evaluated' and r['outcome']=='matched' for r in rows)
    assert stats['diagnostic_errors'] == sum(r['kind']=='diagnostic' and r['outcome']=='error' for r in rows)
    old = Path(str(journal) + '.3'); old.write_bytes(b'broken\n' + old.read_bytes())
    assert run('history').returncode == 3 and run('stats').returncode == 3
    assert run('history', '--tail', '--limit', '1').returncode == 0
    assert run('stats', '--tail').returncode == 1
    for suffix in ('.1','.2','.3'): Path(str(journal)+suffix).unlink()
    for data in (b'\n', b'\x00\n', b'x'*131072+b'\n', b'x'*131073,
                 encode(rows[0]).rstrip(b'\n') + b'{}\n'):
        journal.write_bytes(data)
        assert run('stats').returncode == 3, len(data)
print('journal queries match reference across segments, filters, block limits and partial tails')
