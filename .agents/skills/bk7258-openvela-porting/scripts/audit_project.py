#!/usr/bin/env python3
"""Read-only source signals, never a firmware or hardware acceptance verdict."""

import argparse
import json
import os
from pathlib import Path
import re
import sys


CODE_SUFFIXES = {'.c', '.h', '.cpp', '.cc', '.cxx'}
TRIVIA = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
ENOSYS = re.compile(r'\bENOSYS\b')
GRAPHICS_API = re.compile(r'\b(?:lv|lcd|nx)_[A-Za-z0-9_]+\s*\(')
GRAPHICS_CONFIG = re.compile(
    r'^\s*CONFIG_(?:GRAPHICS|LCD|LVGL)(?:_[A-Z0-9_]+)?=(?:y|m)\s*$', re.M)


def without_trivia(text):
    # Retain newlines so evidence locations refer to the original source.
    return TRIVIA.sub(lambda match: re.sub(r'[^\n]', ' ', match.group()), text)


def evidence(path, root, text, pattern):
    return [
        {'path': path.relative_to(root).as_posix(),
         'line': text.count('\n', 0, match.start()) + 1}
        for match in pattern.finditer(text)
    ]


def read_source(path):
    if path.is_symlink():
        raise ValueError(f'需要人工审查符号链接，未读取：{path}')
    return path.read_text(encoding='utf-8-sig')


def audit(root):
    root = root.resolve(strict=True)
    for name in ('app', 'board', 'app/vision_badge/src'):
        if not (root / name).is_dir() or (root / name).is_symlink():
            raise ValueError(f'缺少或不支持的项目目录：{name}')
    placeholders = {}
    for service in ('vision', 'camera', 'audio'):
        path = root / f'app/vision_badge/src/{service}_service.c'
        content = without_trivia(read_source(path))
        placeholders[service] = evidence(path, root, content, ENOSYS)

    graphics = []
    def walk_error(error):
        raise error

    for subtree in ('app', 'board'):
        for directory, directories, files in os.walk(root / subtree, onerror=walk_error):
            for name in directories:
                if (Path(directory) / name).is_symlink():
                    raise ValueError(f'需要人工审查符号链接目录：{Path(directory) / name}')
            for name in sorted(files):
                path = Path(directory) / name
                if path.suffix in CODE_SUFFIXES:
                    content = without_trivia(read_source(path))
                    graphics.extend(evidence(path, root, content, GRAPHICS_API))
                elif name in ('defconfig', '.config'):
                    graphics.extend(evidence(path, root, read_source(path), GRAPHICS_CONFIG))

    media = placeholders['camera'] + placeholders['audio']
    return {
        'scope': 'static-source-signals-only',
        'capabilities': {
            'graphics': {'status': 'signal-needs-review' if graphics else 'not-evidenced',
                         'evidence': sorted(graphics, key=lambda item: (item['path'], item['line']))},
            'ai': {'status': 'placeholder-signal' if placeholders['vision'] else 'needs-runtime-evidence',
                   'evidence': placeholders['vision']},
            'multimedia': {'status': 'placeholder-signal' if media else 'needs-runtime-evidence',
                           'evidence': media},
        },
        'limitations': [
            '扫描所有候选配置，不代表当前构建已启用；未解析预处理条件。',
            'ENOSYS 是待人工审查信号，可能属于可选或未编译路径。',
            '未发现占位或发现图形 API 均不证明实现完整、构建通过或实机落地。',
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', nargs='?', type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument('--json', action='store_true', help='machine-readable evidence')
    args = parser.parse_args()
    try:
        result = audit(args.root)
    except (OSError, ValueError, UnicodeError) as error:
        print(f'审查失败（不是能力通过）：{error}', file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        for name, capability in result['capabilities'].items():
            print(f"{name}\t{capability['status']}")
            for item in capability['evidence']:
                print(f"  {item['path']}:{item['line']}")
        for limitation in result['limitations']:
            print(limitation)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
