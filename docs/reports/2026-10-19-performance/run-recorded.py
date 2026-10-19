import subprocess,json,pathlib,time,hashlib,platform,statistics,os
root=pathlib.Path('/tmp/collabtext-perf-2026-10-19');repo=pathlib.Path('/home/clinton/dev/collabtext')
env=dict(os.environ,QT_QPA_PLATFORM='offscreen')
methods=['single_replica_throughput','single_replica_large_doc','edit_patterns','edit_sizes','tombstone_degradation','tombstone_degradation_large','multi_replica_convergence','undo_redo_cycle','undo_stack_depth','fragment_proliferation','memory_growth','reconnect_sync_cost']
meta={'revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),'platform':platform.platform(),'cpu':subprocess.check_output(['lscpu'],text=True),'compiler':subprocess.check_output(['c++','--version'],text=True),'flags':'-O3 -DNDEBUG; sanitizers OFF; C++20','filesystem':subprocess.check_output(['stat','-f','-c','%T','/tmp'],text=True).strip(),'harness_sha256':hashlib.sha256((root/'public_api.cpp').read_bytes()).hexdigest(),'methods':methods,'repeats':5,'commands':[]}
(root/'environment.json').write_text(json.dumps(meta,indent=2))
def run(label,argv,timeout=300):
 before=pathlib.Path('/proc/loadavg').read_text().strip();start=time.time()
 with (root/(label+'.log')).open('w') as f:
  try:r=subprocess.run(argv,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=timeout);status=r.returncode
  except subprocess.TimeoutExpired:status='timeout'
 row={'label':label,'argv':argv,'exit_status':status,'wall_seconds':time.time()-start,'load_before':before,'load_after':pathlib.Path('/proc/loadavg').read_text().strip()};meta['commands'].append(row);(root/'environment.json').write_text(json.dumps(meta,indent=2));print(json.dumps(row),flush=True)
 if status!=0:raise RuntimeError(label+' failed; evidence retained')
for repeat in range(1,6):
 run('native-'+str(repeat),[str(root/'build/libs/collabtext/tst_benchmark'),*methods])
 if repeat>1:run('public-'+str(repeat),[str(root/'public_api')])
for count in [512,2048]:run('recovery-'+str(count),[str(root/'build/libs/collabtext/tst_checkpoint_recovery_benchmark'),str(count)],timeout=300)
