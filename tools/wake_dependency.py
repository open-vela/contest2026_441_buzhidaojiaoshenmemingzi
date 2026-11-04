#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Restore/verify optional ML inputs; generated model stays outside source Git."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from restore_workspace import checkout, safe_path

def packed_model(data):
    """Existing diagnostic PackBits format; storage only, weights unchanged."""
    encoded=bytearray();i=0
    while i<len(data):
        run=1
        while i+run<len(data) and run<128 and data[i+run]==data[i]: run+=1
        if run>=3:
            encoded.extend((257-run,data[i]));i+=run;continue
        start=i;i+=run
        while i<len(data) and i-start<128:
            run=1
            while i+run<len(data) and run<128 and data[i+run]==data[i]: run+=1
            if run>=3: break
            i+=min(run,128-(i-start))
        encoded.append(i-start-1);encoded.extend(data[start:i])
    checksum=2166136261
    for value in data: checksum=((checksum^value)*16777619)&0xffffffff
    return b'R1PB'+len(data).to_bytes(4,'little')+checksum.to_bytes(4,'little')+encoded

def source_patch(workspace,item,apply=False):
    patch=item['source_patch'];repo=Path(__file__).resolve().parents[1]
    patch_file=safe_path(repo,patch['path']);path=safe_path(workspace,item['path'])
    if hashlib.sha256(patch_file.read_bytes()).hexdigest()!=patch['sha256']:
        raise RuntimeError('Wake source patch hash differs')
    def git(*args):return subprocess.check_output(['git','-C',str(path),*args],text=True).rstrip('\n')
    if git('rev-parse','HEAD')!=item['commit'] or git('rev-parse','HEAD^{tree}')!=item['tree']:
        raise RuntimeError('Wake patch base commit/tree differs')
    states={name:hashlib.sha256(safe_path(path,name).read_bytes()).hexdigest() for name in patch['files']}
    if apply:
        if any(states[name] not in spec.values() for name,spec in patch['files'].items()):
            raise RuntimeError('Unreviewed content before wake patch')
        before=[name for name,spec in patch['files'].items() if states[name]==spec['before']]
        changed=set(patch['files'])-set(before)
        status=git('status','--porcelain','--untracked-files=all').splitlines()
        if any(line[:2]!=' M' for line in status) or {line[3:] for line in status}!=changed:
            raise RuntimeError('Dirty source outside recorded wake patch')
        includes=['--include='+name for name in before]
        if before:
            subprocess.run(['git','-C',str(path),'apply','--check',*includes,str(patch_file)],check=True)
            subprocess.run(['git','-C',str(path),'apply',*includes,str(patch_file)],check=True)
        states={name:hashlib.sha256(safe_path(path,name).read_bytes()).hexdigest() for name in patch['files']}
    if any(states[name]!=spec['after'] for name,spec in patch['files'].items()):
        raise RuntimeError('Wake source patch contents differ')
    changes=git('status','--porcelain','--untracked-files=all').splitlines()
    if any(line[:2]!=' M' for line in changes) or {line[3:] for line in changes}!=set(patch['files']):
        raise RuntimeError('Unrecorded changes outside the wake source patch')

def verify(workspace, emit_model=None):
    lock = json.loads((Path(__file__).parent/'bk7258/wake.json').read_text())
    for item in [*lock['dependencies'], lock['candidate_model']]:
        path = safe_path(workspace, item['path'])
        if not (path/'.git').exists(): raise RuntimeError('Missing wake dependency: '+item['path'])
        def git(*args): return subprocess.check_output(['git','-C',str(path),*args],text=True).strip()
        if git('rev-parse','HEAD') != item['commit'] or git('rev-parse','HEAD^{tree}') != item['tree']:
            raise RuntimeError('Wake dependency commit/tree differs: '+item['path'])
        if 'source_patch' in item:
            source_patch(workspace,item)
        elif git('status','--porcelain','--untracked-files=all'):
            raise RuntimeError('Wake dependency has local changes: '+item['path'])
        if 'license_file' in item and not (path/item['license_file']).is_file():
            raise RuntimeError('Wake dependency license missing: '+item['path'])
    item = lock['candidate_model']; model_root = workspace/item['path']
    for name, expected in item['files'].items():
        if hashlib.sha256((model_root/name).read_bytes()).hexdigest() != expected:
            raise RuntimeError('Wake asset hash mismatch: '+name)
    meta = json.loads((model_root/item['metadata']).read_text())
    if (meta['model_sha256'] != item['files'][item['model']] or meta['wake_label'] != 'nihao_openvela' or
        meta['frontend'] != 'bkvoice-microfrontend-v1' or meta['input_shape'] != [1,149,40,1]):
        raise RuntimeError('Wake model/frontend contract differs')
    if emit_model:
        data = (model_root/item['model']).read_bytes()
        data = packed_model(data)
        generated = ('/* Generated from locked candidate asset; see tools/bk7258/wake.json. */\n'
                     '#include <stddef.h>\nconst unsigned char r1_wake_model_data[] __attribute__((aligned(16))) = {\n'+
                     '\n'.join(','.join(str(v) for v in data[i:i+20])+',' for i in range(0,len(data),20))+
                     '\n};\nconst size_t r1_wake_model_bytes = sizeof(r1_wake_model_data);\n')
        emit_model.parent.mkdir(parents=True,exist_ok=True)
        if not emit_model.exists() or emit_model.read_text()!=generated: emit_model.write_text(generated)
    print('R1_WAKE_INPUTS_VERIFIED; model is an acoustic candidate, not accepted hardware evidence')
    return lock

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--workspace',type=Path,default=Path(__file__).resolve().parents[2])
    p.add_argument('--restore',action='store_true')
    p.add_argument('--emit-model',type=Path)
    args=p.parse_args()
    lock=json.loads((Path(__file__).parent/'bk7258/wake.json').read_text())
    if args.restore:
        for item in lock['dependencies']:
            path=safe_path(args.workspace,item['path'])
            if 'sparse_paths' in item and not path.exists():
                path.parent.mkdir(parents=True,exist_ok=True)
                subprocess.run(['git','clone','--no-checkout','--filter=blob:none','--single-branch',item['url'],str(path)],check=True)
                subprocess.run(['git','-C',str(path),'sparse-checkout','set','--no-cone',*('/'+n for n in item['sparse_paths'])],check=True)
                subprocess.run(['git','-C',str(path),'fetch','--filter=blob:none','origin',item['commit']],check=True)
                subprocess.run(['git','-C',str(path),'checkout','--detach',item['commit']],check=True)
            patched='source_patch' in item and (path/'.git').exists() and subprocess.check_output(
                ['git','-C',str(path),'status','--porcelain'],text=True).strip()
            if not patched: checkout(args.workspace,item['path'],item['url'],item['commit'],item['tree'])
            if 'source_patch' in item: source_patch(args.workspace,item,apply=True)
        item=lock['candidate_model'];path=safe_path(args.workspace,item['path'])
        if not path.exists():
            path.parent.mkdir(parents=True,exist_ok=True)
            subprocess.run(['git','clone','--no-checkout','--filter=blob:none','--single-branch',item['url'],str(path)],check=True)
            subprocess.run(['git','-C',str(path),'sparse-checkout','set','--no-cone',*('/'+n for n in item['files'])],check=True)
            subprocess.run(['git','-C',str(path),'fetch','--filter=blob:none','origin',item['commit']],check=True)
            subprocess.run(['git','-C',str(path),'checkout','--detach',item['commit']],check=True)
    verify(args.workspace,args.emit_model)

if __name__=='__main__': main()
