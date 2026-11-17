#!/usr/bin/env python3
"""Изолированный Wazuh 4.14.0: правила, ротация, downtime, повторы и реальные alerts.
Нужен официальный .deb, bwrap, cc и dpkg-deb. Действующие службы не изменяются.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

REPO = Path(__file__).resolve().parents[2]
SHA256 = '00a54fce7d2b2d77e4c9ba4352a7ea17297504d1a29eb1fb49c29267e443e6c8'


def inside(binary):
    root = Path('/var/ossec')
    assert (root / '.fileward-isolated-test').read_text() == 'namespace-only\n'
    processes, streams = [], []
    journal = root / 'logs/fileward/events.jsonl'
    replay = root / 'logs/fileward/replay.jsonl'
    alerts = root / 'logs/alerts/alerts.json'
    lab = root / 'fw'; inbox = lab / 'inbox'; staging = lab / 'staging'
    inbox.mkdir(parents=True); staging.mkdir()
    config = lab / 'fileward.conf'
    config.write_text(f'watch {inbox}\nwhen moved_to ** log "delivery test"\n')

    def start(args, name):
        stream = (root / 'logs' / name).open('w'); streams.append(stream)
        process = subprocess.Popen(list(map(str, args)), stdout=stream, stderr=stream)
        processes.append(process)
        return process

    def stop(process):
        if process.poll() is None:
            process.terminate()
            try: process.wait(timeout=4)
            except subprocess.TimeoutExpired: process.kill(); process.wait()

    def rows(path):
        return [json.loads(line) for line in path.read_bytes().splitlines(keepends=True)
                if line.endswith(b'\n')] if path.exists() else []

    def records():
        return [r for suffix in ('.3','.2','.1','') for r in rows(Path(str(journal)+suffix))]

    def wait(predicate, timeout=30):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            result = predicate()
            if result: return result
            time.sleep(.05)
        raise AssertionError('Истекло ожидание Wazuh/Fileward')

    def analysis():
        process = start([root/'bin/wazuh-analysisd','-f'], 'analysisd-test.log')
        wait(lambda: 'Logtest started' in (root/'logs/analysisd-test.log').read_text())
        return process

    def fixtures(lab_enabled):
        subprocess.run([sys.executable, str(REPO/'tests/integration/wazuh-logtest.py'),
                        *(['--lab'] if lab_enabled else [])], check=True, timeout=90)

    def writer(maximum):
        process = start([binary,'run','--config',config,'--journal',journal,
                         '--control-socket',lab/'control.sock','--settle-ms','10',
                         '--journal-max-bytes',str(maximum)], 'fileward-test.log')
        wait(lambda: 'fileward started' in (root/'logs/fileward-test.log').read_text())
        return process

    def publish(name):
        file = staging/name; file.write_bytes(b'x'*2048); file.rename(inbox/name)
        return wait(lambda: next((r for r in records() if r['kind']=='action_result'
                                  and r['relative_path']==name and r['outcome']=='success'), None))

    def delivered(record, location=journal):
        return wait(lambda: next((a for a in rows(alerts) if a.get('data',{}).get('event_id')==record['event_id']
                                  and a.get('location')==str(location)), None))

    try:
        for daemon in ('wazuh-analysisd','wazuh-logcollector'):
            check = subprocess.run([root/'bin'/daemon,'-t'],capture_output=True,text=True)
            assert check.returncode==0 and not any(word in check.stderr for word in ('WARNING','ERROR','CRITICAL')), check.stderr
        start([root/'bin/wazuh-db','-f'], 'db-test.log')
        manager = analysis(); fixtures(False); stop(manager)
        rules = ET.parse(REPO/'integrations/wazuh/local_rules.xml',
                         parser=ET.XMLParser(target=ET.TreeBuilder(insert_comments=True)))
        for node in list(rules.getroot()):
            if node.tag is ET.Comment and '<rule id="100805"' in node.text:
                rules.getroot().append(ET.fromstring(node.text.strip())); rules.getroot().remove(node)
        rules.write(root/'etc/rules/fileward_rules.xml', encoding='unicode')
        check = subprocess.run([root/'bin/wazuh-analysisd','-t'], capture_output=True, text=True)
        assert check.returncode == 0 and not check.stderr.strip(), check.stderr
        manager = analysis(); fixtures(True)
        collector = start([root/'bin/wazuh-logcollector','-f'], 'collector-test.log')
        wait(lambda: 'Analyzing file:' in (root/'logs/collector-test.log').read_text())
        producer = writer(16384)
        rotations = 0; inode = journal.stat().st_ino
        for n in range(8):
            record = publish(f'live-{n}.pdf'); alert = delivered(record)
            assert alert['rule']['id']=='100805' and alert['rule']['groups']==['fileward'], alert
            assert alert['data']['provider']=='fileward' and alert['data']['relative_path']==record['relative_path']
            new_inode = journal.stat().st_ino; rotations += new_inode != inode; inode = new_inode
        assert rotations >= 2, rotations
        # Resume a stopped collector without overflowing retained segments.
        stop(producer); producer = writer(64*1024*1024)
        stop(collector)
        offline = publish('during-downtime.pdf')
        collector = start([root/'bin/wazuh-logcollector','-f'], 'collector-restarted.log')
        delivered(offline)
        # Intentional replay is isolated from the daemon-owned journal.
        with replay.open('ab') as file:
            file.write((json.dumps(offline)+'\n').encode()*2)
        copies = wait(lambda: [a for a in rows(alerts) if a.get('data',{}).get('event_id')==offline['event_id']
                               and a.get('location')==str(replay)] if len([a for a in rows(alerts)
                               if a.get('data',{}).get('event_id')==offline['event_id']
                               and a.get('location')==str(replay)]) >= 2 else None)
        spec = importlib.util.spec_from_file_location('normalize', REPO/'integrations/shuffle/normalize.py')
        normalize = importlib.util.module_from_spec(spec); spec.loader.exec_module(normalize)
        keys = {normalize.normalize(a)['dedup_key'] for a in copies}
        assert len(keys)==1
        stop(producer)
        config.write_text(f'watch {lab}/missing/inbox\nwhen moved_to ** log\n')
        result = subprocess.run([binary,'run','--config',config,'--journal',journal],capture_output=True)
        assert result.returncode==3 and b'FW_WATCHER_START' in result.stderr, result.stderr
        failure = next(r for r in reversed(records()) if r['code']=='FW_WATCHER_START')
        assert delivered(failure)['rule']['id']=='100804'
        print(json.dumps({'wazuh_version':'4.14.0','fixtures_per_profile':12,'live_successes':8,
                          'rotations_observed':rotations,'collector_downtime_caught_up':True,
                          'replay_alerts':len(copies),'stable_dedup_key':True,'startup_failure_rule':'100804',
                          'journal_mode':oct(journal.stat().st_mode & 0o777),'vcheck_files':1}))
    except BaseException:
        for name in ('db-test.log','analysisd-test.log','collector-test.log','collector-restarted.log','fileward-test.log'):
            path=root/'logs'/name
            if path.exists(): print(name+'\n'+'\n'.join(path.read_text().splitlines()[-15:]),file=sys.stderr)
        raise
    finally:
        for process in reversed(processes): stop(process)
        for stream in streams: stream.close()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, help='Официальный wazuh-manager_4.14.0-1_amd64.deb')
    parser.add_argument('--binary', type=Path, default=REPO/'build/fileward')
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    args=parser.parse_args(); binary=args.binary.resolve()
    if args.inside:
        inside(binary); return
    if not args.package or not args.package.is_file(): parser.error('Нужен --package с официальным .deb')
    for tool in ('bwrap','cc','dpkg-deb'):
        if not shutil.which(tool): parser.error('Не найдена программа '+tool)
    with args.package.open('rb') as file:
        if hashlib.file_digest(file,'sha256').hexdigest()!=SHA256: parser.error('SHA256 пакета не совпадает с проверенной версией')
    if not binary.is_file(): parser.error('Сначала соберите Fileward')
    with tempfile.TemporaryDirectory(prefix='fw-wazuh-') as folder:
        temporary=Path(folder); extracted=temporary/'package'
        subprocess.run(['dpkg-deb','--extract',str(args.package.resolve()),str(extracted)],check=True)
        root=extracted/'var/ossec'
        (root/'.fileward-isolated-test').write_text('namespace-only\n')
        passwd='root:x:0:0:root:/root:/bin/sh\nwazuh:x:0:0:Wazuh:/var/ossec:/bin/sh\n'
        group='root:x:0:\nwazuh:x:0:\n'
        for path in (temporary/'passwd',root/'etc/passwd'): path.write_text(passwd)
        for path in (temporary/'group',root/'etc/group'): path.write_text(group)
        for name in ('logs/alerts','logs/archives','logs/fileward','stats/weekly-average','stats/hourly-average','stats/totals','queue/db','var/run'):
            (root/name).mkdir(parents=True,exist_ok=True)
        for name in ('events.jsonl','replay.jsonl'): (root/'logs/fileward'/name).touch(mode=0o600)
        shutil.copy2(REPO/'integrations/wazuh/local_rules.xml',root/'etc/rules/fileward_rules.xml')
        lists=''.join('<list>'+str(f.relative_to(root))+'</list>' for f in (root/'etc/lists').rglob('*')
                      if f.is_file() and not f.name.endswith('.cdb'))
        sources=''
        template=ET.parse(REPO/'integrations/wazuh/agent-localfile.xml').getroot()
        for name in ('events.jsonl','replay.jsonl'):
            localfile=ET.fromstring(ET.tostring(template))
            localfile.find('location').text='/var/ossec/logs/fileward/'+name
            sources+=ET.tostring(localfile,encoding='unicode')
        (root/'etc/ossec.conf').write_text('<ossec_config><global><jsonout_output>yes</jsonout_output><alerts_log>yes</alerts_log>'
            '<email_notification>no</email_notification></global><alerts><log_alert_level>3</log_alert_level></alerts>'
            '<ruleset>'+lists+'<decoder_dir>ruleset/decoders</decoder_dir><rule_dir>ruleset/rules</rule_dir>'
            '<rule_dir>etc/rules</rule_dir></ruleset><rule_test><enabled>yes</enabled><threads>1</threads>'
            '<max_sessions>8</max_sessions></rule_test>'+sources+'</ossec_config>')
        (root/'etc/local_internal_options.conf').write_text('logcollector.vcheck_files=1\nlogcollector.state_interval=1\n'
            +''.join('analysisd.'+name+'=1\n' for name in ('event_threads','syscheck_threads','syscollector_threads',
                 'rootcheck_threads','sca_threads','hostinfo_threads','winevt_threads','rule_matching_threads','dbsync_threads')))
        # Unprivileged user namespaces forbid setgroups. Only this privilege-drop
        # syscall is stubbed for the test; decoders and rule evaluation are stock.
        shim=temporary/'setgroups.c'; shim.write_text('#include <stddef.h>\n#include <sys/types.h>\n'
            'int setgroups(size_t n,const gid_t *g){(void)n;(void)g;return 0;}\n')
        subprocess.run(['cc','-shared','-fPIC','-o',str(root/'lib/fwtest-shim.so'),str(shim)],check=True)
        command=['bwrap','--die-with-parent','--ro-bind','/','/','--unshare-user','--uid','0','--gid','0',
            '--cap-add','CAP_SYS_CHROOT','--unshare-pid','--unshare-net','--unshare-ipc','--proc','/proc','--dev','/dev',
            '--tmpfs','/var','--bind',str(root),'/var/ossec','--ro-bind',str(temporary/'passwd'),'/etc/passwd',
            '--ro-bind',str(temporary/'group'),'/etc/group','--setenv','LD_PRELOAD','/var/ossec/lib/fwtest-shim.so',
            '--setenv','PYTHONDONTWRITEBYTECODE','1','--',sys.executable,str(Path(__file__).resolve()),'--inside','--binary',str(binary)]
        result=subprocess.run(command,text=True,capture_output=True,timeout=300)
        print(result.stdout,end=''); print(result.stderr,end='',file=sys.stderr)
        if result.returncode: raise SystemExit(result.returncode)


if __name__=='__main__': main()
