"""A path the `tur` BINARY can open, not just this interpreter.

Shared by the three DAP drivers, which each hand a program path to `tur dap`
and then wait for it to launch.

Under MSYS2 -- which is how Windows CI runs these -- `os.path.abspath` yields
`/d/a/turmeric/turmeric/tests/fixtures/dap/input.tur`. That is a real file to
the msys runtime and to python, and meaningless to tur.exe, which is a native
Windows program: a leading slash there is the root of the current drive. The
debuggee never launched, so no `stopped` event ever arrived and the driver
timed out on something that looked like a debugger fault.

The translation has to happen on the python side, after abspath. Converting in
the shell first does not work: MSYS python does not consider `C:/a/b` absolute
(no leading slash), so abspath would prepend the working directory to it.

cygpath is the spelling both halves agree on. Absent -- real POSIX, or a native
python with no MSYS2 -- the path is already usable and comes back unchanged.
"""
import os
import subprocess


def native_path(path):
    p = os.path.abspath(path)
    if os.name != "nt" and "MSYSTEM" not in os.environ:
        return p
    try:
        out = subprocess.run(["cygpath", "-m", p], capture_output=True, timeout=15)
        if out.returncode == 0:
            got = out.stdout.decode().strip()
            if got:
                return got
    except (OSError, subprocess.SubprocessError):
        pass
    return p
