from pathlib import Path
import argparse,csv,subprocess
repo=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description='Compare real reference/CMSIS-NN target scores on supplied synthetic PCM clips; no acoustic acceptance claim')
p.add_argument('--pcm-dir',type=Path,required=True)
p.add_argument('--output-dir',type=Path,required=True)
a=p.parse_args();a.output_dir.mkdir(parents=True,exist_ok=True)
for name,hits in (('target',1),('ordinary',0),('near',0)):
    traces=[]
    for backend,folder in (('reference','wake-model-reference'),('cmsis','wake-model')):
        trace=a.output_dir/(name+'-'+backend+'.csv')
        result=subprocess.run([str(repo/'out/host'/folder/'build/test_wake'),str(a.pcm_dir/(name+'.pcm')),str(trace)],check=True,text=True,stdout=subprocess.PIPE)
        assert ('CMSIS_SATURATING_SHIFT_PASS' in result.stdout)==(backend=='cmsis')
        rows=[tuple(map(int,row)) for row in csv.reader(trace.open())]
        assert rows and sum(v[2] for v in rows)==hits,(name,backend,result.stdout)
        traces.append(rows)
    assert len(traces[0])==len(traces[1]),name
    maximum=0
    for left,right in zip(*traces):
        assert left[0]==right[0] and left[2]==right[2],(name,left,right)
        maximum=max(maximum,abs(left[1]-right[1]))
    assert maximum<=1,(name,maximum)
    print('WAKE_BACKEND_PARITY_PASS',name,'windows',len(traces[0]),'triggers',hits,'max_probability_delta',maximum,'/256',flush=True)
print('SYNTHETIC_BACKEND_PARITY_ONLY; not human/board latency acceptance')
