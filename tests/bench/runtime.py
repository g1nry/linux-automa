#!/usr/bin/env python3
"""Сравнение бинарей на одинаковом потоке файлов и 64 правилах; без порога скорости."""
import argparse
import json
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
args = parser.parse_args()
if not 1 <= args.files <= 2000 or not 1 <= args.runs <= 20 or not 1 <= args.workers <= 32:
    parser.error('Нужны --files 1..2000, --runs 1..20, --workers 1..32')
binaries = [str(path.resolve()) for path in args.binaries]
for binary in binaries:
    if not Path(binary).is_file(): parser.error(f'Не найден бинарь: {binary}')
results = {binary: [] for binary in binaries}


def measure(binary):
    with tempfile.TemporaryDirectory(prefix='fw-bench-') as folder:
        root = Path(folder)
        inbox = root/'inbox'; inbox.mkdir()
        staging = root/'staging'; staging.mkdir()
        config = root/'config'
        rules = [f'rule miss-{i} priority={i} name=never-{i} when moved_to ** log "unused"' for i in range(63)]
        rules.append('rule final priority=-1 type=file when moved_to ** log "processed"')
        config.write_text(f'watch {inbox}\n'+'\n'.join(rules)+'\n')
        for number in range(args.files): (staging/f'file-{number}.dat').write_bytes(b'benchmark\n')
        control = root/'control.sock'
        with (root/'output').open('w') as output:
            process = subprocess.Popen([binary,'run','--config',str(config),'--journal',str(root/'events.jsonl'),
                '--control-socket',str(control),'--queue-size','4096','--workers',str(args.workers),'--ready-mode','immediate'],stdout=output,stderr=output)
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
                return elapsed
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
    median = statistics.median(samples)
    print(json.dumps({'binary':binary,'files':args.files,'rules':64,'workers':args.workers,'runs':args.runs,
        'seconds':[round(value,4) for value in samples], 'median_seconds':round(median,4),
        'median_files_per_second':round(args.files/median,2)},ensure_ascii=False))
