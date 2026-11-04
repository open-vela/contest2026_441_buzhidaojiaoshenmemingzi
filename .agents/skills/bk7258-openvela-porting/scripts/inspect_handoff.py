#!/usr/bin/env python3
"""Check a flattened BK7258 CP/AP export; never flash or certify hardware."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


def inspect(root, board, selector=None):
    if root.is_symlink():
        raise ValueError('export directory is a symlink')
    root = root.resolve(strict=True)

    def read(name):
        path = root / name
        if path.is_symlink() or not path.is_file():
            raise ValueError(f'missing or linked export file: {name}')
        return path.read_bytes()

    def require(condition, message):
        if not condition:
            raise ValueError(message)

    def digest(name, expected):
        require(isinstance(expected, str) and re.fullmatch(r'[0-9a-f]{64}', expected),
                f'invalid expected SHA256: {name}')
        data = read(name)
        require(hashlib.sha256(data).hexdigest() == expected, f'hash mismatch: {name}')
        return data

    manifest = json.loads(read('build-manifest.json'))
    record = json.loads(read('source-record.json'))
    require(manifest['format'] == 'bk7258.build-manifest/3', 'unsupported manifest format')
    require(manifest['target'] == {'board_family': 'bk7258', 'physical_board': board},
            'target board mismatch')
    provenance = manifest['provenance']
    require(type(provenance['dirty']) is bool and type(record['source_dirty']) is bool,
            'dirty state must be boolean')
    require(re.fullmatch(r'[0-9a-f]{40}', provenance['source_commit']) is not None,
            'invalid baseline commit')
    require(re.fullmatch(r'[0-9a-f]{64}', provenance['input_tree_sha256']) is not None,
            'missing exact firmware input tree')
    for exported, original in [('source_commit', 'source_commit'),
                               ('source_dirty', 'dirty'),
                               ('input_tree_sha256', 'input_tree_sha256')]:
        require(record[exported] == provenance[original], f'source identity mismatch: {exported}')
    checked = []
    for role in ('boot', 'cp', 'ap', 'pair'):
        name = role + '.bin'
        entry = manifest['finalized_flash'][role]
        data = digest(name, entry['sha256'])
        require(len(data) == entry['size'], f'size mismatch: {name}')
        checked.append(name)
    for role in ('cp', 'ap'):
        name = role + '.elf'
        entry = manifest['elfs'][role]
        data = digest(name, entry['sha256'])
        require(len(data) == entry['size'], f'size mismatch: {name}')
        checked.append(name)
        name = role + '.config'
        config = digest(name, manifest['roles'][role]['resolved_config_sha256']).decode('utf-8')
        values = dict(re.findall(r'^(CONFIG_[A-Z0-9_]+)=(.+)$', config, re.M))
        require(values.get('CONFIG_BK7258_RPTUN') == 'y', f'{role}: RPTUN not enabled')
        if role == 'ap':
            require(values.get('CONFIG_BK7258_AP_CORE') == 'y' and
                    values.get('CONFIG_SMP') == 'y' and values.get('CONFIG_SMP_NCPUS') == '2',
                    'AP must be BK7258 AP with two logical CPUs')
        else:
            require(values.get('CONFIG_BK7258_AP_CORE') != 'y' and
                    values.get('CONFIG_SMP') != 'y' and values.get('CONFIG_SMP_NCPUS') == '1',
                    'CP must be physical CPU0, single-core')
        checked.append(name)
    if selector is not None:
        selection = json.loads(selector.read_bytes())
        require(selection['directory'] == root.name, 'selector export directory mismatch')
        for field in ('source_commit', 'source_dirty', 'input_tree_sha256'):
            require(selection[field] == record[field], f'selector source mismatch: {field}')
    return {
        'scope': 'export-consistency-only', 'status': 'consistent',
        'board': board, 'boot': manifest['boot'],
        'source_commit': provenance['source_commit'], 'source_dirty': provenance['dirty'],
        'input_tree_sha256': provenance['input_tree_sha256'], 'checked_files': checked,
        'profiles': provenance['profiles'],
        'hardware_acceptance': 'not-assessed',
        'export_hardware_annotation': record.get('hardware_status', 'not-recorded'),
        'limitations': ['Hashes detect inconsistent exports, not an authenticated release.',
                        'No ELF semantics, encoded partition geometry, SDK workspace, or device checked.',
                        'Run bk7258.py verify build-manifest in the original workspace separately.',
                        'Hardware evidence must independently match this exact firmware identity.'],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('export', type=Path)
    parser.add_argument('--board', required=True, help='expected physical board identity')
    parser.add_argument('--selector', type=Path, help='optional current-release.json')
    args = parser.parse_args()
    try:
        result = inspect(args.export, args.board, args.selector)
    except (OSError, ValueError, KeyError, TypeError, UnicodeError) as error:
        print(json.dumps({'scope': 'export-consistency-only', 'status': 'invalid',
                          'error': str(error)}, ensure_ascii=False), file=sys.stderr)
        return 2
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
