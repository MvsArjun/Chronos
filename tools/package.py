"""Archive Git-tracked source/evidence, excluding build products and local tools."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import zipfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path, default=root.parent / "chronos-source.zip")
args = parser.parse_args()
paths = sorted(subprocess.check_output(["git", "ls-files", "-z"], cwd=root).decode().rstrip("\0").split("\0"))
if not paths or paths == [""]:
    raise SystemExit("Stage source files with git add before packaging")
manifest = []
with zipfile.ZipFile(args.output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
    for name in paths:
        payload = (root / name).read_bytes()
        manifest.append(hashlib.sha256(payload).hexdigest() + "  " + name)
        info = zipfile.ZipInfo("chronos/" + name, date_time=(2026, 10, 28, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        archive.writestr(info, payload)
    archive.writestr("chronos/MANIFEST.sha256", "\n".join(manifest) + "\n")
print(f"Archived {len(paths)} tracked files into {args.output}")
