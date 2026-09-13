"""Linux integration tests: process groups must not leak between benchmarks."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

RUNNER = Path(__file__).resolve().parents[1] / 'scripts/ci/benchmark_session.py'
SERVER = '''import os,pathlib,signal,time,sys
pid=os.fork()
if pid:
    sys.exit(0)
p=pathlib.Path(sys.argv[1])
def publish(path,value):
    temp=path.with_name(path.name+'.tmp')
    temp.write_text(value)
    temp.replace(path)
def term(signum,frame):
    publish(p.with_suffix('.cleanup'),'ready')
signal.signal(signal.SIGTERM,term)
publish(p,str(os.getpid()))
time.sleep(30)
'''
WORK = '''import os,pathlib,signal,sys,time
p=pathlib.Path(sys.argv[1])
end=time.monotonic()+5
while not p.exists():
    if time.monotonic()>end:sys.exit(9)
    time.sleep(.01)
if sys.argv[2] in ('sleep','orphan'):
    pid=os.fork()
    if not pid:
        signal.signal(signal.SIGTERM,signal.SIG_IGN)
        p.with_suffix('.work').write_text(str(os.getpid()))
        time.sleep(30)
        sys.exit(0)
    while not p.with_suffix('.work').exists():time.sleep(.01)
    if sys.argv[2]=='sleep':time.sleep(30)
    sys.exit(0)
else:sys.exit(int(sys.argv[2]))
'''


def alive(pid):
    try:
        state=Path(f'/proc/{pid}/stat').read_text().rsplit(') ',1)[1].split()[0]
        return state not in ('Z','X')
    except FileNotFoundError:
        return False


@unittest.skipUnless(sys.platform == 'linux', 'Linux process-group contract')
class SessionTests(unittest.TestCase):
    def run_case(self, mode, timeout: float = 8, terminate=False,
                 signal_phase='workload', signal_number=signal.SIGTERM):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            server=root/'server.py';server.write_text(SERVER)
            work=root/'work.py';work.write_text(WORK)
            pidfile=root/'child.pid'
            grace='1' if signal_phase=='cleanup' else '0.1'
            args=[sys.executable,str(RUNNER),'--grace',grace,'--timeout',str(timeout),
                  '--server',f'{sys.executable} {server} {pidfile}',
                  '--',sys.executable,str(work),str(pidfile),mode]
            with (root/'log').open('w+') as log:
                process=subprocess.Popen(args,stdout=log,stderr=log)
                try:
                    if terminate:
                        deadline=time.monotonic()+5
                        marker=pidfile.with_suffix('.cleanup' if signal_phase=='cleanup' else '.work')
                        while not marker.exists() and process.poll() is None:
                            self.assertLess(time.monotonic(),deadline)
                            time.sleep(.01)
                        self.assertTrue(marker.exists())
                        process.send_signal(signal_number)
                    code=process.wait(timeout=12)
                    log.seek(0);output=log.read()
                    self.assertTrue(pidfile.exists(),output)
                    pid=int(pidfile.read_text())
                    self.assertFalse(alive(pid),f'worker {pid} survived: {output}')
                    if mode in ('sleep', 'orphan'):
                        self.assertTrue(pidfile.with_suffix('.work').exists(), output)
                        self.assertFalse(alive(int(pidfile.with_suffix('.work').read_text())), output)
                    return code
                finally:
                    if process.poll() is None:
                        process.kill();process.wait()
                    # Test-fixture recovery only, never used to satisfy assertions.
                    for marker, owner in ((pidfile, server), (pidfile.with_suffix('.work'), work)):
                        if not marker.exists():
                            continue
                        pid=int(marker.read_text())
                        if alive(pid):
                            handle = os.pidfd_open(pid)
                            try:
                                argv = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
                                if os.fsencode(owner) in argv:
                                    signal.pidfd_send_signal(handle, signal.SIGKILL)
                            finally:
                                os.close(handle)

    def test_early_server_exit_does_not_leak_child(self):
        self.assertEqual(self.run_case('0'),0)

    def test_workload_failure_is_preserved(self):
        self.assertEqual(self.run_case('7'),7)

    def test_workload_early_exit_does_not_leak_child(self):
        self.assertEqual(self.run_case('orphan'),0)

    def test_timeout_cleans_server(self):
        self.assertEqual(self.run_case('sleep',timeout=.5),124)

    def test_sigterm_cleans_server(self):
        self.assertEqual(self.run_case('sleep',terminate=True),143)

    def test_unrelated_group_is_not_signalled(self):
        other=subprocess.Popen([sys.executable,'-c','import time;time.sleep(30)'],start_new_session=True)
        try:
            self.assertEqual(self.run_case('0'),0)
            self.assertIsNone(other.poll())
        finally:
            other.terminate();other.wait(timeout=5)

    def test_sigterm_during_cleanup_is_not_success(self):
        self.assertEqual(self.run_case('0',terminate=True,signal_phase='cleanup'),143)

    def test_sigint_during_cleanup_is_not_success(self):
        self.assertEqual(self.run_case('0',terminate=True,signal_phase='cleanup',
                                      signal_number=signal.SIGINT),130)


    def test_final_handler_handoff_preserves_cancellation(self):
        code = '''import importlib.util,os,signal,sys
spec=importlib.util.spec_from_file_location("session",sys.argv[1])
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
received=[]
real=signal.signal
real(signal.SIGINT,lambda number,frame:received.append(number))
real(signal.SIGTERM,lambda number,frame:received.append(number))
def install(number,handler):
    result=real(number,handler)
    if number==signal.SIGINT:os.kill(os.getpid(),signal.SIGTERM)
    return result
signal.signal=install
sys.exit(module.finish_status(0,received))
'''
        result=subprocess.run([sys.executable,'-c',code,str(RUNNER)],timeout=5)
        self.assertEqual(result.returncode,143)

    def test_signal_after_final_status_is_not_success(self):
        code = '''import importlib.util,os,signal,sys
spec=importlib.util.spec_from_file_location("session",sys.argv[1])
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
status=module.finish_status(0,[])
os.kill(os.getpid(),signal.SIGTERM)
sys.exit(status)
'''
        result=subprocess.run([sys.executable,'-c',code,str(RUNNER)],timeout=5)
        self.assertEqual(result.returncode,143)


    def test_first_signal_remains_authoritative(self):
        code = '''import importlib.util,os,signal,sys
spec=importlib.util.spec_from_file_location("session",sys.argv[1])
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
status=module.finish_status(0,[signal.SIGINT])
os.kill(os.getpid(),signal.SIGTERM)
sys.exit(status)
'''
        result=subprocess.run([sys.executable,'-c',code,str(RUNNER)],timeout=5)
        self.assertEqual(result.returncode,130)


if __name__=='__main__':
    unittest.main()
