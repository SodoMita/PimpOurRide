#!/usr/bin/env python3
"""Real uinput -> evdev -> ChromeDrive -> Wayland conformance/latency test.

Run with sudo on a Linux host with /dev/uinput. The script drops back to the
invoking user immediately after creating/chmod-ing the temporary virtual
 device. No repository files or host input devices are modified.
"""
import argparse,csv,fcntl,json,math,os,pwd,random,shutil,signal,socket,statistics,struct,subprocess,sys,time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
UI_SET_EVBIT=0x40045564;UI_SET_KEYBIT=0x40045565;UI_SET_RELBIT=0x40045566
UI_DEV_SETUP=0x405c5503;UI_DEV_CREATE=0x5501;UI_DEV_DESTROY=0x5502
EV_SYN=0;EV_KEY=1;EV_REL=2;SYN_REPORT=0
REL_X=0;REL_Y=1;REL_HWHEEL=6;REL_WHEEL=8
KEY_SCROLLLOCK=70;KEY_F1=59;KEY_F8=66;KEY_O=24;KEY_Z=44;KEY_LEFTMETA=126
BTN_LEFT=0x110;BTN_MIDDLE=0x112

class VirtualInput:
 def __init__(self,name="Arena Virtual Input"):
  self.name=name;self.fd=os.open('/dev/uinput',os.O_WRONLY|os.O_NONBLOCK);self.path=None
  for event in (EV_KEY,EV_REL):fcntl.ioctl(self.fd,UI_SET_EVBIT,event)
  for key in (KEY_SCROLLLOCK,KEY_F1,KEY_F8,KEY_O,KEY_Z,KEY_LEFTMETA,BTN_LEFT,BTN_MIDDLE):fcntl.ioctl(self.fd,UI_SET_KEYBIT,key)
  for rel in (REL_X,REL_Y,REL_HWHEEL,REL_WHEEL):fcntl.ioctl(self.fd,UI_SET_RELBIT,rel)
  setup=struct.pack('=HHHH80sI',6,0x1,0x1,1,name.encode(),0)
  fcntl.ioctl(self.fd,UI_DEV_SETUP,setup);fcntl.ioctl(self.fd,UI_DEV_CREATE)
  deadline=time.monotonic()+5
  while time.monotonic()<deadline:
   for sysdev in Path('/sys/class/input').glob('event*/device/name'):
    try:
     if sysdev.read_text().strip()==name:
      event=sysdev.parts[-3];path=Path('/dev/input')/event
      if path.exists():self.path=path;break
    except (OSError,IndexError):pass
   if self.path:break
   time.sleep(.03)
  if not self.path:raise RuntimeError('uinput device created but its event node did not appear')
  os.chmod(self.path,0o666)
 def event(self,kind,code,value):
  os.write(self.fd,struct.pack('=qqHHi',0,0,kind,code,value))
 def key(self,code,down):self.event(EV_KEY,code,int(down));self.event(EV_SYN,SYN_REPORT,0)
 def tap(self,code):self.key(code,True);time.sleep(.025);self.key(code,False);time.sleep(.06)
 def chord(self):
  self.key(KEY_LEFTMETA,True);time.sleep(.025);self.tap(KEY_SCROLLLOCK);self.key(KEY_LEFTMETA,False);time.sleep(.10)
 def rel(self,code,value):self.event(EV_REL,code,value);self.event(EV_SYN,SYN_REPORT,0)
 def close(self):
  if self.fd>=0:
   try:fcntl.ioctl(self.fd,UI_DEV_DESTROY)
   except OSError:pass
   os.close(self.fd);self.fd=-1

def drop_privileges():
 if os.geteuid()!=0:return
 uid=int(os.environ.get('SUDO_UID',os.getuid()));gid=int(os.environ.get('SUDO_GID',os.getgid()))
 user=pwd.getpwuid(uid);os.initgroups(user.pw_name,gid);os.setgid(gid);os.setuid(uid)
 os.environ.update(HOME=user.pw_dir,USER=user.pw_name,LOGNAME=user.pw_name)

def free_port():
 with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]

def wait_file(path,proc,timeout=25):
 deadline=time.monotonic()+timeout
 while time.monotonic()<deadline:
  if path.exists() and path.stat().st_size:return
  if proc and proc.poll() is not None:raise RuntimeError(f'compositor exited {proc.returncode} before {path.name}')
  time.sleep(.05)
 raise TimeoutError(f'timed out waiting for {path}')

def wait_key_count(path,code,count,proc,timeout=15):
 deadline=time.monotonic()+timeout
 while time.monotonic()<deadline:
  rows=read_rows(path)
  if sum(1 for r in rows if r[0]=='K' and int(r[1])==code)>=count:return
  if proc and proc.poll() is not None:raise RuntimeError(f'compositor exited {proc.returncode} while waiting for input')
  time.sleep(.05)
 raise TimeoutError(f'timed out waiting for {count} key {code} events in {path}')

def read_rows(path):
 try:
  with open(path,newline='') as f:return [r for r in csv.reader(f) if len(r)>=5]
 except FileNotFoundError:return []

def start_session(device,out,tag,wakeup=True,count=0,exec_client=True):
 import tempfile
 runtime=Path(tempfile.mkdtemp(prefix='cd-uinput-runtime-'));os.chmod(runtime,0o700)
 socket_name=f'cd-uinput-{os.getpid()}-{tag}'
 log=out/f'{tag}-client.csv';ready=out/f'{tag}.ready';compositor_log=out/f'{tag}-compositor.log'
 for stale in (ready,log):
  try:stale.unlink()
  except FileNotFoundError:pass
 env=os.environ.copy();env.update(XDG_RUNTIME_DIR=str(runtime),WAYLAND_DISPLAY=socket_name,LIBGL_ALWAYS_SOFTWARE='1')
 cmd=[str(ROOT/'bin/chromedrive'),'--headless','--no-config','--socket',socket_name,'--size','640x400','--fps',str(ARGS.fps),'--vnc-port',str(free_port()),'--input-device',str(device.path),'--grab-input','--input-wakeup' if wakeup else '--no-input-wakeup','--exit-after','90']
 if exec_client:cmd += ['--exec',f"{ROOT/'bin/input-latency-client'} --ready {ready} --log {log} --count {count} --timeout 75"]
 stream=open(compositor_log,'w');proc=subprocess.Popen(cmd,cwd=ROOT,env=env,stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
 try:
  if exec_client:wait_file(ready,proc)
 except Exception:
  proc.terminate();proc.wait(timeout=5);stream.close();raise
 return {'proc':proc,'stream':stream,'runtime':runtime,'socket':socket_name,'env':env,'log':log,'ready':ready,'compositor_log':compositor_log}

def stop_session(session):
 proc=session['proc']
 if proc.poll() is None:
  proc.terminate()
  try:proc.wait(timeout=8)
  except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=3)
 session['stream'].close();shutil.rmtree(session['runtime'],ignore_errors=True)

def grab_test(device,out):
 session=start_session(device,out,'grab',True,2);second=None
 try:
  # Activation; ordinary global keys and modified global chords now belong to the client.
  device.chord();device.tap(KEY_F1)
  device.key(KEY_LEFTMETA,True);time.sleep(.025);device.tap(KEY_O);device.key(KEY_LEFTMETA,False);time.sleep(.08)
  device.tap(KEY_F8)
  device.rel(REL_X,-2000);device.rel(REL_Y,-2000);device.tap(BTN_LEFT);device.tap(BTN_MIDDLE);device.rel(REL_WHEEL,1);time.sleep(.12)
  # Release; this F1 is global and must not appear in the client log.
  device.chord();device.tap(KEY_F1);time.sleep(.10)
  # Reacquire, then make the receiver exit while it is still the grab owner.
  device.chord();device.tap(KEY_Z);wait_key_count(session['log'],KEY_Z,2,session['proc']);time.sleep(.20)
  # A replacement client must see ordinary keys but not the global F1.
  log2=out/'grab-replacement-client.csv';ready2=out/'grab-replacement.ready'
  for stale in (ready2,log2):
   try:stale.unlink()
   except FileNotFoundError:pass
  second=subprocess.Popen([str(ROOT/'bin/input-latency-client'),'--ready',str(ready2),'--log',str(log2),'--count','2','--timeout','30'],cwd=ROOT,env=session['env'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
  wait_file(ready2,session['proc']);device.tap(KEY_F1);device.tap(KEY_Z)
  wait_key_count(log2,KEY_Z,2,session['proc']);second.wait(timeout=5);second=None;time.sleep(.15)
  rows=read_rows(session['log']);rows2=read_rows(log2)
  def keys(rows,code):return [r for r in rows if r[0]=='K' and int(r[1])==code]
  checks={
   'locked F1 press+release delivered':len(keys(rows,KEY_F1))==2,
   'locked F8 press+release delivered':len(keys(rows,KEY_F8))==2,
   'locked Meta+O letter delivered':len(keys(rows,KEY_O))==2,
   'ScrollLock chord never leaked':len(keys(rows,KEY_SCROLLLOCK))==0,
   'outside left click grabbed':sum(1 for r in rows if r[0]=='B' and int(r[1])==BTN_LEFT)==2,
   'middle click grabbed, not look-drag':sum(1 for r in rows if r[0]=='B' and int(r[1])==BTN_MIDDLE)==2,
   'outside wheel axis grabbed':any(r[0]=='A' for r in rows),
   'unmap/destruction released lock':len(keys(rows2,KEY_F1))==0 and len(keys(rows2,KEY_Z))==2,
  }
  for name,ok in checks.items():print(('PASS' if ok else 'FAIL')+': '+name)
  if not all(checks.values()):
   print('First client rows:',rows);print('Replacement rows:',rows2);raise RuntimeError('input grab conformance failed')
  return {'checks':checks,'first_client_events':len(rows),'replacement_events':len(rows2)}
 finally:
  if second and second.poll() is None:second.terminate();second.wait(timeout=3)
  stop_session(session)

def latency_run(device,out,mode,repeat,pairs):
 session=start_session(device,out,f'{mode}-{repeat}',mode=='wakeup',pairs*2)
 try:
  backend_log=session['compositor_log'].read_text()
  if 'kernel-monotonic timestamps' not in backend_log:
   raise RuntimeError('latency measurement requires EVIOCSCLOCKID CLOCK_MONOTONIC; refusing read-time fallback')
  rng=random.Random(0xC0FFEE+repeat*17+(mode=='wakeup'))
  period=1.0/ARGS.fps
  for _ in range(pairs):
   time.sleep(rng.uniform(.001,period+.003));device.key(KEY_Z,True);time.sleep(.001);device.key(KEY_Z,False)
  wait_key_count(session['log'],KEY_Z,pairs*2,session['proc'],30);time.sleep(.15)
  rows=[r for r in read_rows(session['log']) if r[0]=='K' and int(r[1])==KEY_Z]
  values=[]
  for r in rows:
   latency=int(r[4])/1e6-int(r[3])
   if latency < -1 or latency>2000:raise RuntimeError(f'invalid timestamp/latency {latency} ms: {r}')
   values.append(latency)
  if len(values)!=pairs*2:raise RuntimeError(f'{mode}: got {len(values)} samples, expected {pairs*2}')
  return values
 finally:stop_session(session)

def stats(values):
 ordered=sorted(values)
 def percentile(p):
  x=(len(ordered)-1)*p;lo=int(x);hi=min(lo+1,len(ordered)-1);return ordered[lo]+(ordered[hi]-ordered[lo])*(x-lo)
 return {'samples':len(values),'mean_ms':statistics.mean(values),'median_ms':statistics.median(values),'p95_ms':percentile(.95),'p99_ms':percentile(.99),'min_ms':min(values),'max_ms':max(values)}

def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--output',type=Path,required=True);parser.add_argument('--fps',type=int,default=20)
 parser.add_argument('--pairs',type=int,default=100);parser.add_argument('--repeats',type=int,default=2)
 parser.add_argument('--grab-only',action='store_true');parser.add_argument('--benchmark-only',action='store_true')
 global ARGS;ARGS=parser.parse_args()
 if ARGS.grab_only and ARGS.benchmark_only:parser.error('choose at most one mode')
 if ARGS.fps<1 or ARGS.fps>120 or ARGS.pairs<1 or ARGS.repeats<1:parser.error('invalid fps/pairs/repeats')
 device=VirtualInput();result={'method':'uinput kernel input_event.time (EVIOCSCLOCKID CLOCK_MONOTONIC asserted per run) to Wayland client callback; not visual/photon latency','fps_cap':ARGS.fps,'pairs_per_run':ARGS.pairs,'repeats':ARGS.repeats}
 try:
  drop_privileges();ARGS.output.mkdir(parents=True,exist_ok=True)
  subprocess.run(['make','-j2','bin/chromedrive','bin/input-latency-client'],cwd=ROOT,check=True)
  if not ARGS.benchmark_only:result['grab']=grab_test(device,ARGS.output)
  if not ARGS.grab_only:
   all_values={'polling':[],'wakeup':[]};runs=[]
   for repeat in range(ARGS.repeats):
    for mode in ('polling','wakeup'):
     values=latency_run(device,ARGS.output,mode,repeat,ARGS.pairs);all_values[mode]+=values
     row={'mode':mode,'repeat':repeat,**stats(values)};runs.append(row);print(json.dumps(row))
   result['latency']={'runs':runs,'aggregate':{mode:stats(values) for mode,values in all_values.items()}}
   print(json.dumps(result['latency']['aggregate'],indent=2))
  (ARGS.output/'results.json').write_text(json.dumps(result,indent=2)+'\n')
 finally:device.close()

if __name__=='__main__':
 try:main()
 except Exception as exc:
  print(f'ERROR: {exc}',file=sys.stderr);sys.exit(1)
