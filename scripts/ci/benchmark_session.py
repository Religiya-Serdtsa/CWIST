#!/usr/bin/env python3
"""Run one Linux benchmark in private process groups, including its descendants.

Keep leaders unreaped until group cleanup ends, so their numeric PGIDs cannot
be recycled. This supervises trusted benchmark commands; it is not a sandbox
for commands that deliberately escape their session. No command/environment
contents are logged. Requires Linux pidfds and /proc.
"""
import argparse
import math
import os
from pathlib import Path
import select
import shlex
import signal
import subprocess
import sys
import time


def live_members(pgid):
    members = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            fields = (entry / 'stat').read_text().rsplit(') ', 1)[1].split()
        except (FileNotFoundError, ProcessLookupError):
            continue
        if int(fields[2]) == pgid and fields[0] not in ('Z', 'X'):
            members.append(int(entry.name))
    return members


def send_group(pgid, sig):
    try:
        os.killpg(pgid, sig)
    except ProcessLookupError:
        pass


def stop_group(process, grace):
    # Do not poll()/wait() on this leader before the last group signal.
    try:
        send_group(process.pid, signal.SIGTERM)
        try:
            deadline = time.monotonic() + grace
            while live_members(process.pid) and time.monotonic() < deadline:
                time.sleep(.02)
        finally:
            send_group(process.pid, signal.SIGKILL)
        deadline = time.monotonic() + 2
        while live_members(process.pid):
            if time.monotonic() >= deadline:
                raise RuntimeError('benchmark group did not stop')
            time.sleep(.02)
    finally:
        process.wait(timeout=2)


def positive_seconds(value):
    parsed = float(value)
    if not math.isfinite(parsed) or parsed <= 0 or parsed > 600:
        raise argparse.ArgumentTypeError('must be finite and in (0, 600]')
    return parsed


def finish_status(code, received):
    # Cleanup is over. Keep terminal handlers until this standalone CLI exits.
    # Signals during the two-handler handoff are either latched by the old
    # handler and checked below, or terminate immediately through the new one.
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda number, frame: os._exit(128 + (received[0] if received else number)))
    return 128 + received[0] if received else code


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', required=True)
    parser.add_argument('--timeout', type=positive_seconds, default=120)
    parser.add_argument('--grace', type=positive_seconds, default=2)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    server_command = shlex.split(args.server)
    if not command or not server_command:
        parser.error('server and workload commands are required')
    if sys.platform != 'linux' or not hasattr(os, 'pidfd_open'):
        parser.error('Linux pidfds and /proc are required')

    processes = []
    received = []
    reader, writer = os.pipe2(os.O_NONBLOCK | os.O_CLOEXEC)
    old_wakeup = signal.set_wakeup_fd(writer)
    old_child_handler = signal.getsignal(signal.SIGCHLD)
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda number, frame: received.append(number))
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    pidfd = None
    code = 1
    cleanup_errors = []
    try:
        server = subprocess.Popen(server_command, start_new_session=True)
        processes.append(server)
        if not received:
            env = os.environ.copy()
            env['BENCHMARK_SERVER_PID'] = str(server.pid)
            workload = subprocess.Popen(command, env=env, start_new_session=True)
            processes.append(workload)
            pidfd = os.pidfd_open(workload.pid)
            ready, _, _ = select.select([pidfd, reader], [], [], args.timeout)
            if received:
                code = 128 + received[0]
            elif pidfd not in ready:
                code = 124
            else:
                result = os.waitid(os.P_PID, workload.pid, os.WEXITED | os.WNOWAIT)
                code = result.si_status if result.si_code == os.CLD_EXITED else 128 + result.si_status
    finally:
        for process in reversed(processes):
            try:
                stop_group(process, args.grace)
            except Exception as error:
                cleanup_errors.append(type(error).__name__)
        if pidfd is not None:
            os.close(pidfd)
        signal.set_wakeup_fd(old_wakeup)
        signal.signal(signal.SIGCHLD, old_child_handler)
        os.close(reader)
        os.close(writer)
        if cleanup_errors:
            raise RuntimeError('benchmark cleanup failed: ' + ', '.join(cleanup_errors))
    return finish_status(code, received)


if __name__ == '__main__':
    sys.exit(main())
