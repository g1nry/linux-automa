#!/usr/bin/env python3
"""Сравнение бинарей на одинаковом потоке файлов и 64 правилах; без порога скорости."""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binaries', nargs='+', type=Path)
parser.add_argument('--files', type=int, default=300)
parser.add_argument('--runs', type=int, default=3)
parser.add_argument('--workers', type=int, default=4)
parser.add_argument('--rules', type=int, default=64)
parser.add_argument('--journal-sync', action='store_true')
args = parser.parse_args()
if not 1 <= args.files <= 2000 or not 1 <= args.runs <= 20 or not 1 <= args.workers <= 32 or not 1 <= args.rules <= 64:
    parser.error('Нужны --files 1..2000, --runs 1..20, --workers 1..32, --rules 1..64')
binaries = [str(path.resolve()) for path in args.binaries]
for binary in binaries:
    if not Path(binary).is_file(): parser.error(f'Не найден бинарь: {binary}')
if len(set(binaries)) != len(binaries): parser.error('Бинари в сравнении должны различаться')
results = {binary: [] for binary in binaries}


def measure(binary):
    with tempfile.TemporaryDirectory(prefix='fw-bench-') as folder:
        root = Path(folder)
        inbox = root/'inbox'; inbox.mkdir()
        staging = root/'staging'; staging.mkdir()
        config = root/'config'
        rules = [f'rule miss-{i} priority={i} name=never-{i} when moved_to ** log "unused"' for i in range(args.rules-1)]
        rules.append('rule final priority=-1 type=file when moved_to ** log "processed"')
        config.write_text(f'watch {inbox}\n'+'\n'.join(rules)+'\n')
        for number in range(args.files): (staging/f'file-{number}.dat').write_bytes(b'benchmark\n')
        control = root/'control.sock'
        with (root/'output').open('w') as output:
            process = subprocess.Popen([binary,'run','--config',str(config),'--journal',str(root/'events.jsonl'),
                '--control-socket',str(control),'--queue-size','4096','--workers',str(args.workers),'--ready-mode','immediate',*(['--journal-sync'] if args.journal_sync else [])],stdout=output,stderr=output)
            try:
                def health():
                    response = subprocess.run([binary,'health','--socket',str(control)],capture_output=True,text=True,timeout=5)
                    return json.loads(response.stdout) if response.returncode in (0,4) else None
                deadline = time.monotonic()+10
                status = None
                while time.monotonic()<deadline:
                    status = health()
                    if status: break
                    assert process.poll() is None,(root/'output').read_text()
                    time.sleep(.01)
                assert status, 'Не удалось дождаться health'
                began = time.monotonic()
                for file in staging.iterdir(): file.rename(inbox/file.name)
                deadline = began+60
                while time.monotonic()<deadline:
                    status = health()
                    if status and status['chains_completed'] >= args.files: break
                    assert process.poll() is None,(root/'output').read_text()
                    time.sleep(.01)
                elapsed = time.monotonic()-began
                assert status and status['chains_completed']==args.files,status
                assert status['events_dropped']==0 and status['action_failures']==0,status
                # Проверка аудита и чтение /proc выполняются после остановки таймера.
                summary=json.loads(subprocess.check_output([binary,'stats','--journal',str(root/'events.jsonl')],text=True,timeout=15))
                assert summary['events_observed']==args.files and summary['events_processed']==args.files,summary
                assert summary['rules_evaluated']==args.files*args.rules and summary['rules_matched']==args.files,summary
                assert summary['actions_succeeded']==args.files and summary['failures']==0 and summary['malformed_records']==0,summary
                cpu_seconds=peak_rss_kib=None
                try:
                    fields=Path(f'/proc/{process.pid}/stat').read_text().rpartition(') ')[2].split()
                    cpu_seconds=(int(fields[11])+int(fields[12]))/os.sysconf('SC_CLK_TCK')
                    for line in Path(f'/proc/{process.pid}/status').read_text().splitlines():
                        if line.startswith('VmHWM:'): peak_rss_kib=int(line.split()[1])
                except (OSError,ValueError,IndexError): pass
                return {'seconds':elapsed,'cpu_seconds':cpu_seconds,'peak_rss_kib':peak_rss_kib,
                        'audit_records':summary['records'],'rules_evaluated':summary['rules_evaluated'],
                        'actions_succeeded':summary['actions_succeeded']}
            finally:
                process.terminate()
                try: assert process.wait(timeout=5)==0
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(); raise


# Меняем очередность бинарей между прогонами, чтобы не всегда прогревать первый.
for run in range(args.runs):
    for binary in binaries if run%2==0 else reversed(binaries):
        results[binary].append(measure(binary))
for binary, samples in results.items():
    median = statistics.median(sample['seconds'] for sample in samples)
    print(json.dumps({'binary':binary,'files':args.files,'rules':args.rules,'workers':args.workers,
        'journal_sync':args.journal_sync,'runs':args.runs,
        'samples':[{**sample,'seconds':round(sample['seconds'],4)} for sample in samples],
        'median_seconds':round(median,4),'median_files_per_second':round(args.files/median,2)},ensure_ascii=False))
