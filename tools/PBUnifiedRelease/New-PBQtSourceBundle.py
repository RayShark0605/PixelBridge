"""Create-only corresponding-source bundle for the pinned Qt Base installation.

No downloads, subprocesses, or changes to the Qt installation. The complete module
is kept, including bundled third-party sources, licenses, and build scripts.
"""
import argparse
import hashlib
import json
import re
import stat
import zipfile
from pathlib import Path


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def checked_files(root):
    if root.is_symlink() or root.is_junction():
        raise ValueError(f"Reparse source root: {root}")
    files = []
    total = 0
    for path in sorted(root.rglob("*")):
        if path.is_symlink() or path.is_junction():
            raise ValueError(f"Reparse source path: {path}")
        if path.is_file():
            size = path.stat().st_size
            if size > 256 * 1024**2:
                raise ValueError(f"Oversized source file: {path}")
            total += size
            files.append(path)
    if not 1 <= len(files) <= 65536 or total > 1024**3:
        raise ValueError("Qt source count/size outside the release bound")
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-root", required=True, type=Path)
    parser.add_argument("--output-directory", required=True, type=Path)
    args = parser.parse_args()
    qt_root = args.qt_root.resolve(strict=True)
    source = qt_root.parent / "Src" / "qtbase"
    match = re.search(r'QT_REPO_MODULE_VERSION\s+"(\d+\.\d+\.\d+)"',
                      (source / ".cmake.conf").read_text(encoding="utf-8"))
    if not match:
        raise ValueError("Qt Base module version missing")
    version = match[1]
    if version != "6.10.1":
        raise ValueError("This release recipe is pinned to Qt 6.10.1")
    files = checked_files(source)
    entries = [(path, "qtbase/" + path.relative_to(source).as_posix()) for path in files]
    for name in ("config_qtbase.opt", "config_qtbase.summary"):
        path = qt_root / name
        if not path.is_file() or path.is_symlink() or path.is_junction():
            raise ValueError(f"Missing/unsafe Qt build metadata: {name}")
        entries.append((path, "build-info/" + name))
    output = args.output_directory.absolute()
    output.mkdir(parents=True, exist_ok=False)
    archive = output / f"qtbase-{version}-source.zip"
    inventory = []
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
        for path, name in entries:
            before = sha256(path)
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (stat.S_IFREG | 0o644) << 16
            data = path.read_bytes()
            if hashlib.sha256(data).hexdigest() != before:
                raise ValueError(f"Qt source changed while bundling: {name}")
            bundle.writestr(info, data)
            inventory.append(dict(path=name, size=len(data), sha256=before))
    manifest = dict(schema="PixelBridge.QtSourceBundle.1", version=version,
                    module="qtbase", sourceOrigin="Qt Online Installer source component",
                    sourceModifiedByPixelBridge=False, fileCount=len(inventory), files=inventory,
                    archive=dict(path=archive.name, size=archive.stat().st_size, sha256=sha256(archive)))
    (output / "qt-source-manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    with zipfile.ZipFile(archive) as bundle:
        if bundle.testzip() is not None or len(bundle.infolist()) != len(inventory):
            raise ValueError("Qt source archive self-check failed")
    print(json.dumps({key: manifest[key] for key in ("version", "fileCount", "archive")}))


if __name__ == "__main__":
    main()
