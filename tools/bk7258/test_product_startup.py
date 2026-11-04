#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual network/product startup flow; services mocked, no I/O or RTC."""
from pathlib import Path
import subprocess
import tempfile

repo=Path(__file__).resolve().parents[2]
source=(repo/'app/vision_badge/src/vision_badge_rpc.c').read_text()
start=source.index('static int vision_badge_network_worker(')
opening=source.index('{',start);depth=1;end=opening+1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}');end+=1
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <syslog.h>
#define CONFIG_CONTEST2026_441_POWER 1
#define CONFIG_CONTEST2026_441_PRODUCT_STARTUP 1
static bool g_network_power_busy,allowed=true,stop_during_camera;
static int network_calls,failures,camera_calls,wake_calls,camera_error,wake_error,waits;
static bool power_service_work_allowed(void){return allowed;}
static int mimo_http_prepare_session(void){return ++network_calls<=failures ? -ENETDOWN : 0;}
static int nxsig_usleep(unsigned int us){assert(us==2000000);waits++;return 0;}
static int agora_service_command(const char *action,char *text,unsigned long capacity){
 (void)text;(void)capacity;assert(!strcmp(action,"camera-live-on"));camera_calls++;
 if(stop_during_camera)allowed=false;
 return camera_error;
}
static int wake_service_command(const char *action,char *text,unsigned long capacity){
 (void)text;(void)capacity;assert(allowed && !strcmp(action,"start"));wake_calls++;return wake_error;
}
''' + source[start:end] + r'''
static void reset(void){network_calls=failures=camera_calls=wake_calls=camera_error=wake_error=waits=0;
 allowed=true;stop_during_camera=false;g_network_power_busy=true;}
int main(void){
 reset();assert(vision_badge_network_worker(0,0)==0);assert(network_calls==1 && camera_calls==1 && wake_calls==1 && !g_network_power_busy);
 reset();failures=2;vision_badge_network_worker(0,0);assert(network_calls==3 && waits==2 && wake_calls==1);
 reset();failures=3;vision_badge_network_worker(0,0);assert(network_calls==3 && !camera_calls && !wake_calls && !g_network_power_busy);
 reset();allowed=false;vision_badge_network_worker(0,0);assert(!network_calls && !wake_calls && !g_network_power_busy);
 reset();camera_error=-EBUSY;vision_badge_network_worker(0,0);assert(camera_calls==1 && !wake_calls);
 reset();stop_during_camera=true;vision_badge_network_worker(0,0);assert(camera_calls==1 && !wake_calls);
 reset();wake_error=-ECONNREFUSED;vision_badge_network_worker(0,0);assert(wake_calls==1 && !g_network_power_busy);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='r1-product-startup-') as directory:
    path=Path(directory)/'test.c';binary=Path(directory)/'test';path.write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-fno-sanitize-recover=all','-no-pie',str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
    print('PRODUCT_STARTUP_PASS seven readiness/failure/power-cancel/busy cases; actual worker, services mocked; no RTC')
