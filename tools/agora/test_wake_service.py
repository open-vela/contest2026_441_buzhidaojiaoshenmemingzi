#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import subprocess,tempfile
repo=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='r1-wake-service-') as directory:
    root=Path(directory)
    for name,text in {
        'nuttx/config.h':'',
        'nuttx/kthread.h':'int kthread_create(const char*,int,int,int (*)(int,char**),char *const[]);\n',
        'nuttx/mutex.h':'#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\nstatic inline int nxmutex_lock(mutex_t*m){return -pthread_mutex_lock(m);}\nstatic inline int nxmutex_unlock(mutex_t*m){return -pthread_mutex_unlock(m);}\n',
        'nuttx/signal.h':'int nxsig_usleep(unsigned int);\n',
        'arch/chip/bk7258_psram.h':'#include <stddef.h>\nvoid *bk7258_psram_malloc(size_t);\nvoid bk7258_psram_free(void*);\n',
    }.items():
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    scenarios=('stop','trigger','local-trigger','local-stop','manual','power','rpc','overflow','stale-stop','stale-age','model-fail','capture-fail','resume-fail','feed-fail','join-fail','selftest','selftest-fail','frontend-selftest','frontend-selftest-fail')
    for mode,defines in (('up',[]),('smp',['-DCONFIG_SMP=1']),
                         ('asr-up',['-DCONFIG_CONTEST2026_441_WAKE_ASR=1']),
                         ('asr-smp',['-DCONFIG_CONTEST2026_441_WAKE_ASR=1','-DCONFIG_SMP=1']),
                         ('udp-up',['-DCONFIG_CONTEST2026_441_WAKE_ASR=1','-DCONFIG_CONTEST2026_441_WAKE_UDP=1']),
                         ('udp-smp',['-DCONFIG_CONTEST2026_441_WAKE_ASR=1','-DCONFIG_CONTEST2026_441_WAKE_UDP=1','-DCONFIG_SMP=1']),
                         ('product-up',['-DCONFIG_CONTEST2026_441_PRODUCT_STARTUP=1','-DCONFIG_CONTEST2026_441_WAKE_ASR=1','-DCONFIG_CONTEST2026_441_WAKE_UDP=1']),
                         ('product-smp',['-DCONFIG_CONTEST2026_441_PRODUCT_STARTUP=1','-DCONFIG_CONTEST2026_441_WAKE_ASR=1','-DCONFIG_CONTEST2026_441_WAKE_UDP=1','-DCONFIG_SMP=1'])):
        binary=root/('test-'+mode)
        subprocess.run(['cc','-D_DEFAULT_SOURCE','-std=c11','-Wall','-Wextra','-Werror','-g','-O1','-no-pie',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(root),
                        '-I'+str(repo/'app/vision_badge/include'),*defines,str(repo/'tools/agora/test_wake_service.c'),
                        '-pthread','-o',str(binary)],check=True)
        for scenario in (*scenarios,*(('pin-fail',) if '-DCONFIG_SMP=1' in defines else ()),
                         *(('asr-jitter',) if '-DCONFIG_CONTEST2026_441_WAKE_ASR=1' in defines else ()),
                         *(('capture-time',) if '-DCONFIG_CONTEST2026_441_WAKE_UDP=1' in defines else ()),
                         *(('retry-feed','retry-open','retry-busy','retry-backoff','retry-stop','retry-manual','retry-power','retry-device-fault','retry-capture-timeout','retry-local-test','retry-then-wake')
                            if '-DCONFIG_CONTEST2026_441_PRODUCT_STARTUP=1' in defines else ())):
            subprocess.run([str(binary),scenario],check=True,timeout=15)
