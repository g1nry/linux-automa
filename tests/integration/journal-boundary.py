#!/usr/bin/env python3
"""History/stats читают максимальную запись целиком и отклоняют превышение лимита."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1] if len(sys.argv)>1 else 'build/fileward').resolve())
maximum = 131072
with tempfile.TemporaryDirectory(prefix='fw-journal-boundary-') as folder:
    journal = Path(folder)/'events.jsonl'
    row = {'schema_version':1,'kind':'action_result','outcome':'success','code':'FW_OK',
           'event_id':'boundary','parent_id':'','message':''}
    def encode(): return (json.dumps(row,separators=(',',':'))+'\n').encode()
    row['message'] = 'x'*(maximum-len(encode()))
    expected = encode()
    assert len(expected)==maximum
    journal.write_bytes(expected)
    result = subprocess.run([binary,'history','--journal',str(journal)],capture_output=True,timeout=5)
    assert result.returncode==0 and result.stdout==expected,(result.returncode,result.stderr)
    result = subprocess.run([binary,'stats','--journal',str(journal)],capture_output=True,timeout=5)
    assert result.returncode==0 and json.loads(result.stdout)['actions_succeeded']==1
    row['message'] += 'x'
    journal.write_bytes(encode())
    for command in ('history','stats'):
        result = subprocess.run([binary,command,'--journal',str(journal)],capture_output=True,timeout=5)
        assert result.returncode==3 and b'FW_JOURNAL_READ' in result.stderr,(command,result.returncode,result.stderr)
print('maximum journal record and oversized input checks passed')
