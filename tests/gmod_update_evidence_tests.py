"""Exercise evidence collection against a fake install, without a game server."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

bash = sys.argv[1] if len(sys.argv) > 1 else "bash"
script = Path("tools/capture-gmod-update-evidence.sh").resolve()

def shell_path(path):
    if sys.platform == "win32":
        return subprocess.check_output(
            [bash, "-c", 'cygpath -u "$1"', "_", path.as_posix()], text=True
        ).strip()
    return path.as_posix()

with tempfile.TemporaryDirectory(prefix="gmod-evidence-") as temporary:
    base = Path(temporary)
    root = base / "server with spaces"
    output = base / "evidence"
    library = root / "bin" / "linux64" / "engine.so"
    library.parent.mkdir(parents=True)
    library.write_bytes(b"candidate-engine")
    (root / "garrysmod").mkdir()
    (root / "garrysmod" / "garrysmod.ver").write_text("PatchVersion=2026.09.12\n")
    (root / "garrysmod" / "server.cfg").write_text("private-token-must-not-appear")
    (root / "steamapps").mkdir()
    (root / "steamapps" / "appmanifest_4020.acf").write_text(
        '"buildid" "123456"\n"LastOwner" "private-account-must-not-appear"\n'
    )
    before = {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()}
    result = subprocess.run(
        [bash, shell_path(script), shell_path(root), shell_path(output)],
        capture_output=True, text=True, check=True,
    )
    manifest = (output / "native-sha256.txt").read_text()
    assert hashlib.sha256(b"candidate-engine").hexdigest() in manifest
    assert "bin/linux64/engine.so" in manifest
    identity = (output / "engine-identity.txt").read_text()
    assert "2026.09.12" in identity and "123456" in identity
    all_output = "".join(p.read_text() for p in output.iterdir())
    assert "private-token" not in all_output and "private-account" not in all_output
    rejected = root / "must-not-create"
    failure = subprocess.run(
        [bash, shell_path(script), shell_path(root), shell_path(rejected)],
        capture_output=True, text=True,
    )
    assert failure.returncode == 2 and not rejected.exists()
    after = {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()}
    assert before == after, "Evidence collector changed the server installation"
print("Evidence collector: hashes, identity, privacy and read-only boundary passed")
