#!/usr/bin/env python3
"""Validate shipped XML and the pure Shuffle adapter without live services."""
import importlib.util
from pathlib import Path
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[2]
for file in (root / 'integrations/wazuh').glob('*.xml'):
    ET.parse(file)
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
print('integration fixtures passed (live Wazuh/Shuffle not required)')
