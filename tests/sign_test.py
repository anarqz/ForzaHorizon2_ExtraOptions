"""Builds tests/sign_test.c and runs it: adapters/azimuth_sign.c on the RFC 8032 Ed25519 vectors,
on tampered signatures, and on a sample DLL signed by ../tools/sign_release.sh with the real
release key (skipped when that key is not on this machine: FH2_SIGNING_KEY, by default
~/.config/forzainfinity/release-ed25519.pem).
Usage: python tests/sign_test.py <zig>
Built for the host; with FH2_RUN set (the command that runs a Windows program, e.g. a Proton
wine: FH2_RUN="/path/to/Proton/files/bin/wine") it is built for Windows and run through it."""
import hashlib, os, pathlib, shlex, subprocess, sys, tempfile

hook = pathlib.Path(__file__).resolve().parent.parent
sign_release = hook.parent / "tools" / "sign_release.sh"
zig = sys.argv[1]
run = shlex.split(os.environ.get("FH2_RUN", ""))
key = pathlib.Path(os.environ.get("FH2_SIGNING_KEY", pathlib.Path.home() / ".config/forzainfinity/release-ed25519.pem"))
with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    exe = tmp / ("sign_test.exe" if run else "sign_test")
    target = ["-target", "x86_64-windows-gnu"] if run else []
    subprocess.run([zig, "cc", *target, "-O1", "-Wall", "-Wextra", "-Werror", str(hook / "tests" / "sign_test.c"),
                    "-o", str(exe)], check=True)
    arguments = []
    if key.exists() and sign_release.exists():
        sample = tmp / "azimuth-tls.dll"
        sample.write_bytes(b"MZ" + os.urandom(4094) + b"+fh2online.99")
        subprocess.run([str(sign_release), str(sample)], check=True)
        arguments = [hashlib.sha256(sample.read_bytes()).hexdigest(), str(sample) + ".sig"]
    sys.exit(subprocess.run(run + [str(exe), *arguments]).returncode)
