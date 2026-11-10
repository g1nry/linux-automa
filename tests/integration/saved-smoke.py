#!/usr/bin/env python3
"""Закрытие писателя, атрибуты аудита, saved, reload и атомарное сохранение."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/fileward').resolve())
with tempfile.TemporaryDirectory(prefix='fw-saved-') as folder:
    root = Path(folder)
    inbox = root / 'inbox'; inbox.mkdir()
    output = root / 'copies'
    config = root / 'fileward.conf'
    journal = root / 'events.jsonl'
    config.write_text(f'watch {inbox}\nignore **/*.tmp\n'
        f'rule save type=file conflict=rename when saved **/*.txt copy {output}\n'
        'rule atomic type=file when moved_to **/*.txt log "atomic publication"\n')

    def records():
        if not journal.exists(): return []
        return [json.loads(line) for line in journal.read_bytes().splitlines(keepends=True) if line.endswith(b'\n')]

    with (root / 'output').open('w') as log:
        process = subprocess.Popen([binary,'run','--config',str(config),'--journal',str(journal),'--settle-ms','50'],stdout=log,stderr=log)

        def wait(predicate):
            deadline = time.monotonic() + 6
            while time.monotonic() < deadline:
                if predicate(): return
                assert process.poll() is None, (root / 'output').read_text()
                time.sleep(.01)
            raise AssertionError((root / 'output').read_text())

        def results():
            return [r for r in records() if r['kind']=='action_result' and r['event_type']=='saved' and r['relative_path']=='report.txt']

        try:
            wait(lambda: any(r['kind']=='daemon_started' for r in records()))
            report = inbox / 'report.txt'
            with report.open('wb') as writer:
                writer.write(b'first'); writer.flush()
                wait(lambda: any(r['kind']=='file_observed' and r['relative_path']=='report.txt' for r in records()))
                time.sleep(.25)
                assert not results() and not output.exists()
                writer.write(b'-complete')
            wait(lambda: len(results())==1)
            assert (output/'report.txt').read_bytes()==b'first-complete'
            closed = [r for r in records() if r['kind']=='file_closed' and r['relative_path']=='report.txt']
            assert closed and closed[-1]['file_size']==14 and closed[-1]['file_mode'] is not None
            assert results()[0]['outcome']=='success'
            # Writable close is observable even without any write: do not claim a content diff.
            with report.open('r+b'): pass
            wait(lambda: len(results())==2)
            assert (output/'report.txt.1').read_bytes()==b'first-complete'
            # Editor-style atomic replacement is moved_to, not a fabricated close on final name.
            temporary = inbox / '.editor.tmp'
            temporary.write_bytes(b'atomic replacement')
            os.replace(temporary, report)
            wait(lambda: any(r['kind']=='action_result' and r['rule_name']=='atomic' for r in records()))
            assert len(results())==2 and report.read_bytes()==b'atomic replacement'
            # A writer open during reload dispatches saved under the generation active at close.
            with report.open('ab') as writer:
                writer.write(b'-after-reload'); writer.flush()
                config.write_text(f'watch {inbox}\nrule new when saved ** log "new generation"\n')
                reply=subprocess.check_output([binary,'reload','--socket',str(journal)+'.sock'],text=True)
                assert json.loads(reply)['config_generation']==2
            wait(lambda: any(r['kind']=='action_result' and r['event_type']=='saved' and r['config_generation']==2 for r in records()))
            assert any(r['kind']=='file_observed' and r['event_type']=='saved' and r['config_generation']==2 for r in records())
        finally:
            process.terminate()
            assert process.wait(timeout=5)==0
print('saved events, close metadata, atomic replacement and reload checks passed')
