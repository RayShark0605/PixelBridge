"""Run exactly six read-only geometry samples against sealed Step3-B inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

BASE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(BASE/'tools/PBRemoteThroughputStep3B'))
from run import local, read_json, rows, identity, require, write_json, MIB
from process_runner import invoke


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=local, required=True)
    parser.add_argument('--exe', type=local, required=True)
    parser.add_argument('--ambiguity', action='store_true', help='Only the separately approved ordinal-15 ambiguity supplement')
    args = parser.parse_args()
    root, exe = args.root, args.exe
    destination = root/('ambiguity-01' if args.ambiguity else 'run-01')
    require((root/'context/start.json').is_file() and not destination.exists(), 'Explicit new run required')
    if args.ambiguity:
        decision = read_json(root/'context/ambiguity-decision.json')
        require(decision['additionalCpuPixelSamples'] == 1 and decision['bootstrapRsCrcPermitted'], 'Explicit ambiguity scope required')
    old = BASE/'artifacts/remote-step3b-20260908-run01'
    start = read_json(root/'context/start.json')
    require(start['approvedSamples']['totalCpuGeometrySamples'] == 6, 'Approved observation count')
    require(identity(Path(start['priorSourceIdentity']))['sha256'] == start['priorSourceIdentitySha256'], 'Prior source identity changed')
    for row in start['sourceFiles']:
        actual = identity(BASE/row['path'])
        require(actual['sha256'] == row['sha256'] and actual['bytes'] == row['bytes'], 'Prior source changed: '+row['path'])
    inputs = [old/'frozen-pixels/source.bgra', old/'codec-v2-01/channel.mkv']
    frozen = [read_json(old/'context/frozen-input.json'), read_json(old/'codec-v2-01/bitstream-identity.json')]
    require([identity(path) for path in inputs] == frozen, 'Sealed pixel/media identity changed')
    source_files = [identity(path) for path in sorted(Path(__file__).parent.iterdir()) if path.is_file()]
    before_exe = identity(exe)
    write_json(root/('context/probe-ambiguity-runtime-start.json' if args.ambiguity else 'context/probe-runtime-start.json'), {'exe': before_exe, 'toolSources':source_files,
        'runtimeFiles':[identity(path) for path in sorted(exe.parent.iterdir()) if path.suffix.lower() in ('.exe','.dll')],
        'inputFiles':frozen, 'priorRunManifest':identity(old/'FINAL_MANIFEST.json')})
    records = [read_json(path) for path in (root/'logs').glob('*.process.json')]
    spent = sum(row['processingSeconds'] for row in records)
    require(spent < 120, 'Cumulative process budget exhausted')
    argv = [exe,'--ambiguity',inputs[1],destination] if args.ambiguity else [exe,'--run',*inputs,destination]
    result = invoke(argv,root/('logs/probe-ambiguity-01.log' if args.ambiguity else 'logs/probe-01.log'),min(60,120-spent),
        output_limits=[(destination/'samples.jsonl',MIB),(destination/'media-prefix.jsonl',MIB)])
    require(not result['failures'] and result['returnCode'] == 0, 'Probe failed; retain partial evidence and stop')
    require(identity(exe) == before_exe and [identity(path) for path in inputs] == frozen, 'Inputs/runtime changed during probe')
    require(source_files == [identity(path) for path in sorted(Path(__file__).parent.iterdir()) if path.is_file()], 'Probe source changed during execution')
    if args.ambiguity:
        samples = rows(destination/'samples.jsonl',2)
        require(len(samples) == 2 and [row['candidateOrdinal'] for row in samples] == [0,1], 'Both original candidates required')
        truth = rows(old/'codec-v2-01/replay/frames.jsonl')[15]
        pixels = rows(old/'codec-v2-01/replay/pixels.jsonl')
        require(all(row['ordinal'] == 15 and row['pixelBlake3'] == pixels[15]['pixelBlake3'] and row['pts'] == 1000 for row in samples), 'Additional pixel scope/provenance')
        summary = read_json(destination/'summary.json')
        require(summary['originalGeometry'][:4] == truth['geometryFit'] and not summary['resolverAccepted'] and summary['genericBootstrapAccepted'] and
                summary['acceptedCandidateCount'] == 1 and summary['originalMarkerCandidates'] == 5 and summary['originalGeometryCandidates'] <= 2, 'Original ambiguity decision mismatch')
        media = rows(destination/'media-prefix.jsonl',16)
        require(len(media) == 16 and all(row == {'ordinal':ordinal,'pts':pixels[ordinal]['pts'],'pixelBlake3':pixels[ordinal]['pixelBlake3']} for ordinal,row in enumerate(media)), 'Supplement media prefix mismatch')
        write_json(root/'AMBIGUITY_RUN_CHECK.json', {'schema':'PixelBridge.GeometryCodec.AmbiguityCheck.1','passed':True,'additionalPixelSamples':1,'candidateRows':2,
            'additionalMediaFrames':16,'otherFiveSamplesRepeated':False,'matchesSealedWARPGeometryAndPixels':True,'nativeResolverRejected':True,
            'originalGenericBootstrapAccepted':True,'candidateSelectionNotBasedOnExpectedPayloadOrGeometry':True,
            'cumulativeSupervisedSecondsIncludingBuilds':spent+result['processingSeconds'],'observedNativePeakJobCommitBytes':result['peakJobCommitBytes'],
            'native512MiBProcessCap':True,'supervisorProcessAndJobCapBytes':2147483648,'newPayloadFecOrReceiverCalls':0,'newWarpObservations':0})
        print('PASS: original ordinal-15 ambiguity resolved without a new admission candidate')
        return
    samples = rows(root/'run-01/samples.jsonl',6)
    require([(row['kind'],row['ordinal']) for row in samples] == [(kind,index) for kind in ('raw','codec') for index in (0,1,15)], 'Sample scope/order changed')
    raw_pixels = rows(old/'raw-full/pixels.jsonl')
    codec_pixels = rows(old/'codec-v2-01/replay/pixels.jsonl')
    for sample in samples:
        expected = (raw_pixels if sample['kind']=='raw' else codec_pixels)[sample['ordinal']]
        require(sample['pixelBlake3'] == expected['pixelBlake3'] and sample['pts'] == expected['pts'], 'Sample pixel/PTS differs from authoritative replay')
        trace = rows(old/('raw-full/frames.jsonl' if sample['kind']=='raw' else 'codec-v2-01/replay/frames.jsonl'))[sample['ordinal']]
        require(sample['reason'] == 'MeasuredOnly_NoAdmissionCandidate' and sample['originalRefineMatched'] and sample['originalCoverageMatched'], 'Probe did not match original functions')
        require(sample['finalGeometry'][:4] == trace['geometryFit'] and sample['resolverAccepted'] == trace['bootstrapAccepted'], 'Actual native locator/resolver differs from sealed WARP trace')
    media = rows(root/'run-01/media-prefix.jsonl',16)
    require(len(media) == 16, 'Media prefix count changed')
    for ordinal, row in enumerate(media):
        require(row == {'ordinal':ordinal,'pts':codec_pixels[ordinal]['pts'],'pixelBlake3':codec_pixels[ordinal]['pixelBlake3']}, 'Decoded prefix provenance changed')
    summary = read_json(root/'run-01/summary.json')
    require(summary['samples'] == 6 and summary['decodedMediaFrames'] == 16 and summary['intentionalPrefix'] and not summary['eofClaimed'], 'Prefix/scope report changed')
    require(all(summary[key] == 0 for key in ('newWarpObservations','newBootstrapFecCalls','newPayloadFecOrReceiverCalls')), 'Unexpected decoding scope')
    require(summary['media']['sourceBlake3'] == read_json(old/'codec-v2-01/replay/summary.json')['media']['sourceBlake3'], 'Original media decoder read different container')
    write_json(root/'RUN_CHECK.json', {'schema':'PixelBridge.GeometryCodec.RunCheck.1','passed':True,'samples':6,
        'decodedMediaFrames':16,'allPixelsPtsAndFinalFitsMatchSealedReplay':True,'nativeResolverResultsMatch':True,
        'newWarpObservations':0,'newBootstrapFecCalls':0,'newPayloadFecOrReceiverCalls':0,'inputsUnchanged':True,
        'cumulativeSupervisedSecondsIncludingBuild':spent+result['processingSeconds'],
        'native512MiBProcessCap':True,'supervisorProcessAndJobCapBytes':2147483648,
        'observedNativePeakJobCommitBytes':result['peakJobCommitBytes']})
    print('PASS: six geometry samples; all sixteen media frames match sealed pixels; no FEC/Receiver/WARP')


if __name__ == '__main__':
    main()
