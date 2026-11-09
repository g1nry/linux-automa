"""Validate a Fileward record or Wazuh alert and produce workflow input.

This pure adapter sends nothing. Persistent deduplication belongs to the workflow.
"""
import json
import sys

ALERT_KINDS = {'action_result', 'event_dropped', 'event_cancelled', 'config_reload',
               'watcher_error', 'runtime_error', 'state_error', 'config_load'}


def normalize(payload):
    if not isinstance(payload, dict):
        raise ValueError('expected an object')
    record = payload.get('data', payload)
    if not isinstance(record, dict) or record.get('provider') != 'fileward':
        raise ValueError('expected Fileward data')
    if str(record.get('schema_version')) != '1':
        raise ValueError('unsupported schema_version')
    for field, maximum in (('event_id', 80), ('host_id', 80), ('kind', 64), ('code', 80)):
        if not isinstance(record.get(field), str) or not 0 < len(record[field]) <= maximum:
            raise ValueError('invalid ' + field)
    if record['kind'] not in ALERT_KINDS:
        raise ValueError('record is not actionable for this workflow')
    dry_run = record.get('dry_run', False)
    if type(dry_run) is not bool:
        if dry_run in ('true', '1', 1): dry_run = True
        elif dry_run in ('false', '0', 0): dry_run = False
        else: raise ValueError('invalid dry_run')
    return {
        'dedup_key': 'fileward:' + record['host_id'] + ':' + record['event_id'],
        'event_id': record['event_id'], 'source_event_id': record.get('parent_id', ''),
        'host_id': record['host_id'], 'host_name': record.get('host_name', ''),
        'kind': record['kind'], 'outcome': record.get('outcome', ''), 'code': record['code'],
        'rule_name': record.get('rule_name', ''), 'action': record.get('action', ''),
        'relative_path': record.get('relative_path', ''),
        'path_base64': record.get('path_base64', ''), 'watch_root': record.get('watch_root', ''),
        'timestamp': record.get('timestamp', ''), 'dry_run': dry_run,
    }


if __name__ == '__main__':
    try:
        print(json.dumps(normalize(json.load(sys.stdin)), ensure_ascii=False))
    except (ValueError, TypeError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(2)
