#!/usr/bin/env python3
"""End-to-end action contracts through the real watcher, reload and state journal."""
import gzip
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/fileward").resolve())


def check(condition, message):
    if not condition:
        raise AssertionError(message)


with tempfile.TemporaryDirectory(prefix="fileward actions ") as temporary:
    lab = Path(temporary)
    inbox = lab / "inbox"
    (inbox / "sub").mkdir(parents=True)
    config = lab / "fileward.conf"
    log = lab / "run.log"
    state = lab / "state"
    helper = lab / "exec helper.py"
    helper.write_text('''import json, subprocess, sys, time
from pathlib import Path
if sys.argv[1] == "record":
    Path(sys.argv[2]).write_text(json.dumps(sys.argv[3:]))
elif sys.argv[1] in ("hang", "background"):
    child = subprocess.Popen(["/bin/sleep", "30"])
    Path(sys.argv[2]).write_text(str(child.pid))
    if sys.argv[1] == "hang": time.sleep(30)
''')
    process = None
    stream = None

    def logs():
        return log.read_text(errors="replace") if log.exists() else ""

    def wait_for(predicate, timeout=6):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            if process is not None and process.poll() is not None:
                raise AssertionError("watcher exited unexpectedly\n" + logs())
            time.sleep(0.02)
        raise AssertionError("timed out\n" + logs())

    def start(dry=False):
        global process, stream
        stream = log.open("w")
        arguments = [binary, "run", "--journal", str(lab / "events.jsonl"), "--config", str(config), "--state-file", str(state)]
        if dry:
            arguments.append("--dry-run")
        process = subprocess.Popen(arguments, cwd=lab, stdout=stream, stderr=stream)
        wait_for(lambda: "fileward started" in logs())

    def stop():
        global process, stream
        if process is not None:
            process.terminate()
            try:
                check(process.wait(timeout=4) == 0, logs())
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise
            finally:
                process = None
                stream.close()
                stream = None

    def write_config(rules):
        config.write_text(f"watch {inbox}\n" + rules)

    def reload(rules):
        previous = logs().count("reloaded config:")
        write_config(rules)
        process.send_signal(1)
        wait_for(lambda: logs().count("reloaded config:") > previous)

    def emit(relative, data=b"payload\n"):
        staged = lab / "staged"
        staged.write_bytes(data)
        staged.rename(inbox / relative)

    def journal():
        return state.read_bytes() if state.exists() else b""

    def child_stopped(pid):
        status = Path(f"/proc/{pid}/stat")
        try:
            return status.read_text().rsplit(")", 1)[1].split()[0] == "Z"
        except (FileNotFoundError, ProcessLookupError):
            return True

    name = "payload name;$(touch INJECTED).txt"
    relative = "sub/" + name
    arguments_file = lab / "argv.json"
    pipeline = (
        f'rule copier priority=30 conflict=rename retries=2 backoff=1ms when moved_to ** copy "{lab}/copied/{{ext}}/{{dir}}"\n'
        f'rule zipper priority=20 when moved_to ** archive "{lab}/archives/{{date}}"\n'
        f'rule runner priority=10 timeout=2s when moved_to ** exec {sys.executable} "{helper}" record "{arguments_file}" "{{path}}" "{{name}}" "" "$HOME"\n'
        f'rule mover stop when moved_to ** move "{lab}/moved"\n'
    )
    try:
        write_config(pipeline)
        subprocess.run([binary, "validate", "--config", str(config)], check=True, stdout=subprocess.DEVNULL)
        preview = inbox / "sub" / name
        preview.write_bytes(b"preview")
        explained = subprocess.check_output([binary, "explain", "--config", str(config), "--event", "moved_to", str(preview)], text=True)
        for action in ("copy", "archive", "exec", "move"):
            check(f"action: {action}" in explained, explained)
        preview.unlink()
        start()
        emit(relative)
        moved = lab / "moved" / name
        copied = lab / "copied/txt/sub" / name
        wait_for(lambda: moved.exists() and journal().count(b"\tmove\n") == 1)
        check(copied.read_bytes() == b"payload\n", "copy contents")
        archives = list((lab / "archives").glob("*/*.gz"))
        check(len(archives) == 1, "archive output")
        with gzip.open(archives[0], "rb") as compressed:
            check(compressed.read() == b"payload\n", "gzip round trip")
        check(json.loads(arguments_file.read_text()) == [str(inbox / relative), name, "", "$HOME"], "argv must remain literal")
        check(not (lab / "INJECTED").exists(), "shell syntax must never execute")
        check(not (inbox / relative).exists(), "move removes source")
        for action in ("copy", "archive", "exec", "move"):
            check(journal().count(f"\t{action}\n".encode()) == 1, "journal action names")

        # Existing archive remains intact; all-mode continues after an action error.
        emit(relative, b"second\n")
        wait_for(lambda: "rule 'mover': action failed" in logs() and journal().count(b"\texec\n") == 2)
        check((lab / "copied/txt/sub" / (name + ".1")).read_bytes() == b"second\n", "rename collision policy")
        check(moved.read_bytes() == b"payload\n", "default move must not overwrite")
        check((inbox / relative).read_bytes() == b"second\n", "failed move retains source")
        check(journal().count(b"\tarchive\n") == 1, "failed archive not journaled")
        check(journal().count(b"\tmove\n") == 1, "failed move not journaled")

        # Invalid action options preserve the entire prior config on reload.
        write_config('rule invalid timeout=0 when moved_to ** exec /bin/true\n')
        validation = subprocess.run([binary, "validate", "--config", str(config)], capture_output=True)
        check(validation.returncode == 2, "invalid timeout must reject config")
        process.send_signal(1)
        wait_for(lambda: "keeping previous rules" in logs())
        emit("retained.txt")
        wait_for(lambda: (lab / "moved/retained.txt").exists())

        reload(f'rule skipped conflict=skip when moved_to ** copy "{lab}/copied/txt/sub"\n')
        previous_state = journal()
        emit(relative, b"skip me\n")
        wait_for(lambda: "skipped '" in logs())
        check((inbox / relative).read_bytes() == b"skip me\n", "skip retains source")
        check(journal() == previous_state, "skip is not a successful action")
        check(copied.read_bytes() == b"payload\n", "skip preserves destination")

        reload('rule failure when moved_to ** exec /bin/false\n')
        emit("failure.txt")
        wait_for(lambda: "rule 'failure': action failed" in logs())
        check(journal() == previous_state, "failed exec not journaled")

        child_file = lab / "child.pid"
        reload(f'rule background timeout=2s when moved_to ** exec {sys.executable} "{helper}" background "{child_file}"\n')
        before_execs = journal().count(b"\texec\n")
        emit("background.txt")
        wait_for(lambda: journal().count(b"\texec\n") == before_execs + 1)
        wait_for(lambda: child_stopped(int(child_file.read_text())))
        previous_state = journal()
        child_file.unlink()
        reload(f'rule timeout timeout=1s when moved_to ** exec {sys.executable} "{helper}" hang "{child_file}"\n')
        emit("timeout.txt")
        wait_for(lambda: "timed out" in logs() and "rule 'timeout': action failed" in logs())
        wait_for(lambda: child_stopped(int(child_file.read_text())))
        check(journal() == previous_state, "timed-out exec not journaled")

        # Shutdown interrupts a running exec and kills its process group.
        child_file.unlink()
        reload(f'rule shutdown timeout=30s when moved_to ** exec {sys.executable} "{helper}" hang "{child_file}"\n')
        emit("shutdown.txt")
        wait_for(child_file.exists)
        child = int(child_file.read_text())
        began = time.monotonic()
        stop()
        check(time.monotonic() - began < 3, "shutdown must interrupt exec")
        check(child_stopped(child), "shutdown must stop descendants")

        # Exercise actual cross-filesystem publication when a second mount exists.
        if os.access("/dev/shm", os.W_OK) and os.stat("/dev/shm").st_dev != os.stat(lab).st_dev:
            with tempfile.TemporaryDirectory(prefix="fileward-exdev-", dir="/dev/shm") as destination:
                write_config(f'rule cross conflict=fail when moved_to ** move "{destination}"\n')
                start()
                emit("cross-device.txt", b"cross-device payload")
                target = Path(destination) / "cross-device.txt"
                wait_for(lambda: target.exists() and not (inbox / "cross-device.txt").exists())
                stop()
                check(target.read_bytes() == b"cross-device payload", "actual EXDEV move contents")
                check(journal().count(b"\tmove\n") == 3, "cross-device success journal")

        # A fresh dry-run performs none of the four side effects.
        write_config(pipeline.replace(str(lab / "copied"), str(lab / "dry-copy"))
                     .replace(str(lab / "archives"), str(lab / "dry-archive"))
                     .replace(str(lab / "moved"), str(lab / "dry-move"))
                     .replace(str(arguments_file), str(lab / "dry-argv.json")))
        previous_state = journal()
        start(dry=True)
        emit("dry.txt")
        wait_for(lambda: "dry-run: would move" in logs())
        stop()
        for name in ("dry-copy", "dry-archive", "dry-move", "dry-argv.json"):
            check(not (lab / name).exists(), "dry-run side effect: " + name)
        check((inbox / "dry.txt").exists(), "dry-run retains source")
        check(journal() == previous_state, "dry-run not journaled")
        check(not list(lab.rglob(".fileward-*")), "temporary outputs must be cleaned up")
    finally:
        stop()

print("actions smoke test passed")
