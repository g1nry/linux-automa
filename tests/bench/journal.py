#!/usr/bin/env python3
"""Сравнение запросов на одинаковом JSONL; генерация и сверка не входят в таймер."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import tempfile
import time

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('binaries', nargs='+', type=Path)
parser.add_argument('--records', type=int, default=100000)
parser.add_argument('--runs', type=int, default=5)
args=parser.parse_args()
if not 100 <= args.records <= 500000 or not 1 <= args.runs <= 20: parser.error('records=100..500000, runs=1..20')
repo=Path(__file__).resolve().parents[2]
base=json.loads((repo/'tests/fixtures/wazuh/cases.json').read_text())[0]['record']
jobs=[]
for binary in args.binaries:
    binary=str(binary.resolve())
    version=subprocess.check_output([binary,'--version'],text=True).strip()
    help_text=subprocess.run([binary,'history','--help'],capture_output=True,text=True,check=True).stdout
    for command in ('stats','history',*(['tail'] if '--tail' in help_text else [])):
        jobs.append({'binary':binary,'version':version,'command':command,'samples':[]})
with tempfile.TemporaryDirectory(prefix='fw-journal-bench-') as folder:
    path=Path(folder)/'events.jsonl'
    expected=[]; total=0
    streams=[Path(str(path)+suffix).open('wb') for suffix in ('.3','.2','.1','')]
    try:
        for n in range(args.records):
            row=dict(base,sequence=n,event_id=f'bench-{n}',parent_id='',message='x'*400,
                     kind='action_result' if n%4==0 else 'rule_evaluated',
                     outcome='success' if n%4==0 else 'skipped')
            line=(json.dumps(row,separators=(',',':'),ensure_ascii=False)+'\n').encode()
            streams[min(n*4//args.records,3)].write(line); total+=len(line)
            if n%4==0: expected.append(line); expected=expected[-20:]
    finally:
        for stream in streams: stream.close()
    for run in range(args.runs):
        for job in jobs if run%2==0 else reversed(jobs):
            command='stats' if job['command']=='stats' else 'history'
            flags=[] if command=='stats' else ['--limit','20',*(['--tail'] if job['command']=='tail' else [])]
            started=time.perf_counter()
            result=subprocess.run([job['binary'],command,'--journal',str(path),*flags],capture_output=True,timeout=60)
            elapsed=time.perf_counter()-started
            assert result.returncode==0,result.stderr
            if command=='history': assert result.stdout==b''.join(expected)
            else:
                stats=json.loads(result.stdout)
                assert stats['records']==args.records and stats['malformed_records']==0,stats
                assert stats['actions_succeeded']==(args.records+3)//4,stats
                assert stats['rules_evaluated']==args.records-(args.records+3)//4,stats
            job['samples'].append(elapsed)
    for job in jobs:
        print(json.dumps({'version':job['version'],'command':job['command'],'records':args.records,
                          'bytes':total,'runs':args.runs,'median_seconds':statistics.median(job['samples']),
                          'samples_seconds':job['samples']},ensure_ascii=False))
