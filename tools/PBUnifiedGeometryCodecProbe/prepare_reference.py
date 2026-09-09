"""Append a read-only probe to the exact sealed locator, not a replacement locator."""
import argparse
import hashlib
import json
from pathlib import Path
import difflib


def write_new(path, text):
    data = text.encode('utf-8')
    if path.exists():
        if path.read_bytes() != data:
            raise ValueError('Different generated source exists; use a new build')
        return
    with path.open('xb') as stream:
        stream.write(data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads((args.repo/'artifacts/remote-step3b-20260908-run01/SOURCE_IDENTITY_FINAL.json').read_text(encoding='utf-8'))
    relative = 'libs/PBModulation/src/local_desktop_decode.cpp'
    expected = next(row['sha256'] for row in manifest['files'] if row['path'] == relative)
    data = (args.repo/relative).read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError('Sealed locator changed')
    original = data.decode('utf-8-sig').replace('\r\n', '\n')
    derived = original + '\n#include "probe.h"\n#include "probe_impl.inc"\n'
    args.output.mkdir(parents=True, exist_ok=True)
    write_new(args.output/'locator_probe.cpp', derived)
    write_new(args.output/'locator_probe.diff', ''.join(difflib.unified_diff(original.splitlines(True), derived.splitlines(True), fromfile=relative, tofile='tool-only/locator_probe.cpp')))
    write_new(args.output/'generation.json', json.dumps({'schema':'PixelBridge.GeometryCodec.Source.1','originalSha256':expected,
        'derivedSha256':hashlib.sha256(derived.encode()).hexdigest(),'unmodifiedOriginalPrefix':True,'admissionCandidate':False},indent=2)+'\n')


if __name__ == '__main__':
    main()
