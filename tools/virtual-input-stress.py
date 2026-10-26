#!/usr/bin/env python3
"""Stress ChromeDrive's eventfd dispatch with independent 16 kHz uinput devices.

Each report contains one EV_REL and one SYN_REPORT, so 16,000 reports/s is
32,000 raw input_event records/s per device. The test activates the real
Meta+ScrollLock grab, injects a fixed number of reports round-robin across
independent kernel devices, and checks client delivery, queue overflow,
kernel-to-client latency, and continuing Wayland frame callbacks.

Run with sudo on Linux with /dev/uinput. Devices are created and chmod-ed
before dropping back to the invoking user. No host input device is modified.
"""
import argparse,csv,importlib.util,json,os,re,shutil,statistics,struct,subprocess,sys,tempfile,time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('virtual_input_test',ROOT/'tools/virtual-input-test.py')
vit=importlib.util.module_from_spec(spec);spec.loader.exec_module(vit)
REPORT=struct.Struct('=qqHHiqqHHi').pack(0,0,vit.EV_REL,vit.REL_X,1,0,0,vit.EV_SYN,vit.SYN_REPORT,0)


def rows_of(path,kind=None):
 rows=vit.read_rows(path)
 return [r for r in rows if kind is None or r[0]==kind]


def stop_session(session):
 proc=session['proc']
 if proc.poll() is None:
  proc.terminate()
  try:proc.wait(timeout=8)
  except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=3)
 session['stream'].close();shutil.rmtree(session['runtime'],ignore_errors=True)


def stats(values):
 if not values:return None
 ordered=sorted(values)
 def pct(p):
  x=(len(ordered)-1)*p;lo=int(x);hi=min(lo+1,len(ordered)-1)
  return ordered[lo]+(ordered[hi]-ordered[lo])*(x-lo)
 return {'samples':len(values),'mean_ms':statistics.mean(values),'median_ms':statistics.median(values),
         'p95_ms':pct(.95),'p99_ms':pct(.99),'min_ms':min(values),'max_ms':max(values)}


def run(args):
 devices=[];session=None;result={
  'method':'real uinput EV_REL+SYN_REPORT at kernel input_event.time (EVIOCSCLOCKID CLOCK_MONOTONIC) to locked Wayland pointer-motion callback; not visual/photon latency',
  'device_count':args.devices,'target_reports_per_second_per_device':args.rate,
  'target_raw_events_per_second_per_device':args.rate*2,'duration_seconds':args.duration,
  'fps_cap':args.fps,'dispatch':'eventfd' if args.wakeup else 'frame-paced',
  'cpu_count':os.cpu_count(),
  'scope':'shared-host results are diagnostic, not a hardware-independent throughput guarantee; loss is separated into kernel SYN_DROPPED and userspace queue overflow',
 }
 try:
  for i in range(args.devices):devices.append(vit.VirtualInput(f'Arena 16kHz Stress {os.getpid()} {i}'))
  vit.drop_privileges();args.output.mkdir(parents=True,exist_ok=True)
  subprocess.run(['make','-j2','bin/chromedrive','bin/chromedrive-demo','bin/input-latency-client'],cwd=ROOT,check=True)
  runtime=Path(tempfile.mkdtemp(prefix='cd-input-stress-runtime-'));os.chmod(runtime,0o700)
  socket_name=f'cd-stress-{os.getpid()}'
  log=args.output/'client.csv';ready=args.output/'ready';compositor_log=args.output/'compositor.log'
  for stale in (ready,log,compositor_log):
   try:stale.unlink()
   except FileNotFoundError:pass
  env=os.environ.copy();env.update(XDG_RUNTIME_DIR=str(runtime),WAYLAND_DISPLAY=socket_name,LIBGL_ALWAYS_SOFTWARE='1')
  cmd=[str(ROOT/'bin/chromedrive'),'--headless','--no-config','--socket',socket_name,
       '--size','640x400','--fps',str(args.fps),'--vnc-port',str(vit.free_port()),
       '--input-wakeup' if args.wakeup else '--no-input-wakeup','--grab-input','--exit-after','120']
  for d in devices:cmd+=['--input-device',str(d.path)]
  cmd+=['--exec',f"{ROOT/'bin/input-latency-client'} --ready {ready} --log {log} --count 0 --log-motions --frame-log --timeout 110"]
  stream=open(compositor_log,'w')
  proc=subprocess.Popen(cmd,cwd=ROOT,env=env,stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
  session={'proc':proc,'stream':stream,'runtime':runtime}
  vit.wait_file(ready,proc,30)
  deadline=time.monotonic()+5
  while 'kernel-monotonic timestamps' not in compositor_log.read_text():
   if proc.poll() is not None:raise RuntimeError('compositor exited before monotonic timestamp confirmation')
   if time.monotonic()>deadline:raise RuntimeError('EVIOCSCLOCKID CLOCK_MONOTONIC was not confirmed')
   time.sleep(.02)

  # Prove that the grab is active before measuring pointer traffic. Locked F1
  # must reach the client instead of opening the compositor help panel.
  devices[0].chord();devices[0].tap(vit.KEY_F1)
  vit.wait_key_count(log,vit.KEY_F1,2,proc,10);time.sleep(.15)
  before_motions=len(rows_of(log,'M'));backend_log_before=compositor_log.read_text()
  start_ns=time.monotonic_ns();start=start_ns/1e9
  total=args.devices*args.rate*args.duration
  if total!=int(total):raise ValueError('devices * rate * duration must be an integer')
  total=int(total);period=1.0/(args.devices*args.rate);next_report=start
  injected=[0]*args.devices
  for step in range(total):
   delay=next_report-time.monotonic()
   if delay>0.00025:time.sleep(delay-0.00015)
   while time.monotonic()<next_report:pass
   index=step%args.devices
   written=os.write(devices[index].fd,REPORT)
   if written!=len(REPORT):raise RuntimeError(f'short uinput report write: {written}/{len(REPORT)}')
   injected[index]+=1;next_report+=period
  end_ns=time.monotonic_ns();elapsed=end_ns/1e9-start
  # The sentinel is ordered after injection. Allow any older reports on other
  # kernel nodes to drain as well; no new motion is generated after this point.
  time.sleep(.10);devices[0].tap(vit.KEY_Z)
  vit.wait_key_count(log,vit.KEY_Z,2,proc,30);time.sleep(.50)
  if proc.poll() is not None:raise RuntimeError(f'compositor exited during stress with status {proc.returncode}')

  rows=rows_of(log);motions=[r for r in rows if r[0]=='M'];stress_motions=motions[before_motions:]
  frames=[r for r in rows if r[0]=='F' and start_ns<=int(r[2])<=end_ns]
  backend_log=compositor_log.read_text()
  stress_backend_log=backend_log[len(backend_log_before):] if backend_log.startswith(backend_log_before) else backend_log
  overflows=[int(x) for x in re.findall(r'input queue overflow \((\d+) event\(s\) discarded\)',stress_backend_log)]
  syn_dropped=len(re.findall(r'\[evdev\] SYN_DROPPED',stress_backend_log))
  latencies=[]
  for row in stress_motions:
   latency=int(row[2])/1e6-int(row[1])
   if latency < -1 or latency>10000:raise RuntimeError(f'invalid motion timestamp/latency {latency} ms: {row}')
   latencies.append(latency)
  injected_total=sum(injected);delivered=len(stress_motions)
  actual_rates=[count/elapsed for count in injected]
  frame_fps=len(frames)/elapsed
  result.update({
   'injected_reports_per_device':injected,'injected_reports_total':injected_total,
   'injection_elapsed_seconds':elapsed,'actual_reports_per_second_per_device':actual_rates,
   'actual_reports_per_second_aggregate':injected_total/elapsed,
   'actual_raw_events_per_second_aggregate':injected_total*2/elapsed,
   'delivered_pointer_motions':delivered,'delivery_ratio':delivered/injected_total,
   'queue_overflow_count':len(overflows),'raw_events_discarded_by_overflow':sum(overflows),
   'kernel_syn_dropped_count':syn_dropped,
   'frame_callbacks_during_injection':len(frames),'frame_callbacks_per_second':frame_fps,
   'latency':stats(latencies),
  })
  result['checks']={
   'injector sustained at least 95% of requested per-device rate':all(r>=args.rate*.95 for r in actual_rates),
   'every injected report reached the locked client':delivered==injected_total,
   'no bounded-queue overflow':not overflows,
   'no kernel evdev SYN_DROPPED':syn_dropped==0,
   'frame callbacks continued during injection':len(frames)>0,
  }
  result['passed']=all(result['checks'].values())
  return result
 finally:
  if session:stop_session(session)
  for d in devices:d.close()


def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--output',type=Path,required=True);p.add_argument('--devices',type=int,default=4)
 p.add_argument('--rate',type=int,default=16000,help='reports/s per device; raw evdev records/s is twice this')
 p.add_argument('--duration',type=int,default=2,help='integer seconds per device schedule')
 p.add_argument('--fps',type=int,default=20);p.add_argument('--no-input-wakeup',dest='wakeup',action='store_false')
 p.set_defaults(wakeup=True);args=p.parse_args()
 if not 1<=args.devices<=32 or not 1<=args.rate<=100000 or not 1<=args.duration<=30 or not 1<=args.fps<=120:
  p.error('require devices 1..32, rate 1..100000, duration 1..30, fps 1..120')
 result=run(args);args.output.mkdir(parents=True,exist_ok=True)
 (args.output/'results.json').write_text(json.dumps(result,indent=2)+'\n')
 print(json.dumps(result,indent=2))
 if not result['passed']:raise RuntimeError('high-rate input stress checks failed; see results.json and compositor.log')

if __name__=='__main__':
 try:main()
 except Exception as exc:print(f'ERROR: {exc}',file=sys.stderr);sys.exit(1)
