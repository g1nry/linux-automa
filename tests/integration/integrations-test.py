#!/usr/bin/env python3
"""Validate shipped XML and the pure Shuffle adapter without live services."""
import importlib.util
import base64
import json
from pathlib import Path
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[2]
for file in (root / 'integrations/wazuh').glob('*.xml'):
    ET.parse(file)
rules_file = root / 'integrations/wazuh/local_rules.xml'
rules = ET.parse(rules_file).getroot().findall('rule')
assert [r.attrib['id'] for r in rules] == [str(n) for n in range(100800, 100805)]
for rule in rules[1:]:
    assert rule.findtext('if_sid') == '100800,86600'
    fields = {f.attrib['name']: f.text for f in rule.findall('field')}
    assert fields['provider'] == '^fileward$' and fields['schema_version'] == '^1$'
# Неактивный лабораторный пример находится после родителей в том же файле.
with_comments = ET.parse(rules_file, parser=ET.XMLParser(target=ET.TreeBuilder(insert_comments=True)))
lab = [ET.fromstring(c.text.strip()) for c in with_comments.getroot()
       if c.tag is ET.Comment and '<rule id="100805"' in c.text]
assert len(lab) == 1 and lab[0].findtext('if_sid') == '100800,86600'
assert {f.attrib['name']: f.text for f in lab[0].findall('field')} == {
    'provider': '^fileward$', 'schema_version': '^1$', 'kind': '^action_result$', 'outcome': '^success$'}
cases = json.loads((root / 'tests/fixtures/wazuh/cases.json').read_text())
schema = json.loads((root / 'documentation/events.schema.json').read_text())
for case in cases:
    record = case['record']
    if case['name'] not in ('suricata-alert', 'short-fileward-json'):
        assert set(schema['required']) <= set(record), case['name']
        assert record['timestamp'] and 'event_type' in record
        assert 'path_base64' in record and 'file_size' in record
encoded = next(c['record'] for c in cases if c['name'] == 'base64-success')
assert base64.b64decode(encoded['path_base64']) == b'encoded-\xff.txt'
spec = importlib.util.spec_from_file_location('normalize', root / 'integrations/shuffle/normalize.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
record = {'provider': 'fileward', 'schema_version': 1, 'event_id': 'run-1', 'parent_id': 'run-0',
          'host_id': 'host', 'kind': 'action_result', 'outcome': 'failure', 'code': 'FW_EXEC_EXIT'}
assert module.normalize({'data': record})['dedup_key'] == 'fileward:host:run-1'
assert module.normalize(record)['source_event_id'] == 'run-0'
assert module.normalize(dict(record, dry_run='false'))['dry_run'] is False
assert module.normalize(dict(record, dry_run='true'))['dry_run'] is True
for payload in ([], {}, {'data': {}}, dict(record, schema_version=2), dict(record, kind='diagnostic')):
    try:
        module.normalize(payload)
    except ValueError:
        pass
    else:
        raise AssertionError(payload)
print('integration fixtures passed (live rule routing: tests/integration/wazuh-logtest.py)')
