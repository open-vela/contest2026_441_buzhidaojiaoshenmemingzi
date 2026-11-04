#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile the actual CP CLI and validate arguments through its RPC boundary."""
from pathlib import Path
import subprocess
import tempfile

repo=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='r1-product-cli-') as directory:
    binary=Path(directory)/'cli'
    subprocess.run(['cc','-std=c11','-D_DEFAULT_SOURCE','-DCONFIG_CONTEST2026_441_POWER=1',
        '-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-no-pie',
        '-I'+str(repo/'tools/xiaozhi/audio-test-stubs'),'-I'+str(repo/'app/vision_badge/include'),
        str(repo/'tools/agora/test_product_cli.c'),'-o',str(binary)],check=True)
    cases=[(['audio-diag','volume'],'audio volume')]
    cases += [(['audio-diag','volume',str(i)],'audio volume '+str(i)) for i in range(1,11)]
    cases += [(['audio-diag','volume',s],'audio volume '+s) for s in ('up','down','reset')]
    cases += [(['audio-diag',s],'audio '+s) for s in ('dac','mic','mem')]
    cases += [(['power',s],'power '+s) for s in ('status','prepare','resume','toggle')]
    for args,expected in cases:
        result=subprocess.run([str(binary),*args],capture_output=True,text=True)
        assert result.returncode==0 and 'RPC_CAPTURE '+expected+'\n' in result.stdout,(args,result.stderr)
    invalid=[['audio-diag','volume',s] for s in ('0','11','-1','01','1.0','up extra','')]
    invalid += [['audio-diag','volume','1','extra'],['audio-diag','dac','up'],['audio-diag','unknown']]
    for args in invalid:
        result=subprocess.run([str(binary),*args],capture_output=True,text=True)
        assert result.returncode!=0 and 'RPC_CAPTURE' not in result.stdout,(args,result.stderr)
    print(f'PRODUCT_CLI_PASS {len(cases)} valid/{len(invalid)} invalid; real CP parser, RPC mocked; no device/cloud')
