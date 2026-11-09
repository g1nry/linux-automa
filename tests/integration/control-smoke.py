#!/usr/bin/env python3
"""Живой health, подтверждение reload и ограниченный управляющий протокол."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1] if len(sys.argv)>1 else 'build/fileward').resolve())
with tempfile.TemporaryDirectory(prefix='fw-control-') as folder:
    root=Path(folder)
    inbox=root/'inbox'; inbox.mkdir()
    config=root/'config'; config.write_text(f'watch {inbox}\nwhen created ** log "old"\n')
    control=root/'control.sock'; journal=root/'events.jsonl'
    def query(command='health', expect=0):
        result=subprocess.run([binary,command,'--socket',str(control)],capture_output=True,text=True,timeout=5)
        assert result.returncode==expect,(result.returncode,result.stdout,result.stderr)
        return json.loads(result.stdout)
    with (root/'output').open('w') as output:
        process=subprocess.Popen([binary,'run','--config',str(config),'--journal',str(journal),'--control-socket',str(control)],stdout=output,stderr=output)
        clients=[]
        try:
            deadline=time.monotonic()+5
            while not control.exists() and time.monotonic()<deadline:
                assert process.poll() is None
                time.sleep(.02)
            status=query()
            schema=json.loads((Path(__file__).resolve().parents[2]/'documentation/health.schema.json').read_text())
            assert all(field in status for field in schema['required'])
            for field, description in schema['properties'].items():
                if description.get('type')=='integer': assert type(status[field]) is int and status[field]>=0
                if description.get('type')=='boolean': assert type(status[field]) is bool
            assert status['schema_version']==1 and status['provider']=='fileward'
            assert status['status']=='healthy' and status['workers']==4 and status['config_generation']==1,status
            assert (control.stat().st_mode & 0o777)==0o600
            # Медленный клиент не блокирует наблюдение или другие запросы.
            for _ in range(8):
                client=socket.socket(socket.AF_UNIX); client.connect(str(control)); clients.append(client)
            assert query()['status']=='healthy'
            config.write_text(f'watch {inbox}\nwhen created ** log "new"\n')
            assert query('reload')['config_generation']==2
            config.write_text('invalid config\n')
            assert query('reload',expect=2)['code']=='FW_RELOAD_REJECTED'
            assert query()['config_generation']==2 and process.poll() is None
            config.write_text(f'watch {inbox}\nwhen created ** log "new"\n')
            request=socket.socket(socket.AF_UNIX); request.settimeout(2); request.connect(str(control)); request.sendall(b'execute /bin/false\n')
            assert b'FW_CONTROL_REQUEST' in request.recv(4096); request.close()
            (inbox/'still-live').write_text('data')
            deadline=time.monotonic()+5
            while 'action log: new' not in (root/'output').read_text() and time.monotonic()<deadline: time.sleep(.02)
            assert 'action log: new' in (root/'output').read_text()
        finally:
            for client in clients: client.close()
            process.terminate(); assert process.wait(timeout=5)==0
    assert not control.exists()
    result=subprocess.run([binary,'health','--socket',str(control)],capture_output=True,timeout=5)
    assert result.returncode==3
    # Переполнение фиксируется в live health, даже когда очередь затем освобождена.
    config.write_text(f'watch {inbox}\nwhen created ** exec "/bin/sleep 5" timeout=10\n')
    with (root/'overload-output').open('w') as output:
        process=subprocess.Popen([binary,'run','--config',str(config),'--journal',str(journal),
            '--control-socket',str(control),'--workers','1','--queue-size','1','--ready-mode','immediate'],stdout=output,stderr=output)
        try:
            deadline=time.monotonic()+5
            while not control.exists() and time.monotonic()<deadline: time.sleep(.02)
            assert query()['status']=='healthy'
            for number in range(30): (inbox/f'overload-{number}').write_text('x')
            deadline=time.monotonic()+5
            while time.monotonic()<deadline:
                result=subprocess.run([binary,'health','--socket',str(control)],capture_output=True,text=True,timeout=5)
                if result.returncode==4: break
                time.sleep(.02)
            assert result.returncode==4,(result.stdout,result.stderr)
            status=json.loads(result.stdout)
            assert status['events_dropped']>0 and status['code']=='FW_HEALTH_DEGRADED'
        finally:
            process.terminate(); assert process.wait(timeout=5)==0
    # Конфликт имени сокета не удаляет обычный файл пользователя.
    control.write_text('keep')
    result=subprocess.run([binary,'run','--config',str(config),'--journal',str(journal),'--control-socket',str(control)],capture_output=True,timeout=5)
    assert result.returncode==3 and control.read_text()=='keep'
print('control smoke test passed')
