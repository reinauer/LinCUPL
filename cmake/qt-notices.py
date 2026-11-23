"""Collect Qt license texts and third-party notices from matching sources."""
import json
from pathlib import Path
import shutil
import sys

source = Path(sys.argv[1]).resolve()
destination = Path(sys.argv[2])


def copy(path):
    path = path.resolve()
    target = destination / path.relative_to(source)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(path, target)


licenses = list(source.rglob("LICENSES"))
if not licenses:
    raise SystemExit("Qt source license directories not found")
for directory in licenses:
    for path in directory.rglob("*"):
        if path.is_file():
            copy(path)
for manifest in source.rglob("qt_attribution.json"):
    if "src" not in manifest.relative_to(source).parts:
        continue
    copy(manifest)
    # Qt's attribution files allow literal newlines inside quoted strings.
    entries = json.loads(manifest.read_text(encoding="utf-8"), strict=False)
    for entry in entries if isinstance(entries, list) else [entries]:
        files = entry.get("LicenseFile", [])
        for name in [files] if isinstance(files, str) else files:
            copy(manifest.parent / name)
