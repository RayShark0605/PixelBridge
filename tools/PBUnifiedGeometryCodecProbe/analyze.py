"""Evidence-only attribution. Expected probe failures remain failures, not PASS runs."""
import argparse
import copy
import json
import math
from pathlib import Path
import sys

BASE = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(BASE/'tools/PBRemoteThroughputStep3B'))
from run import local, rows, read_json, read_bytes, write_json, identity, require, MIB


def ordered_fit(edges):
    require(len(edges) == 24, 'Exactly 24 axis edges')
    logical_mean = measured_mean = 0.0
    for edge in edges:
        logical_mean += edge[0]
        measured_mean += edge[5]
    logical_mean /= 24
    measured_mean /= 24
    numerator = denominator = 0.0
    for edge in edges:
        numerator += (edge[0]-logical_mean)*(edge[5]-measured_mean)
        denominator += (edge[0]-logical_mean)*(edge[0]-logical_mean)
    require(denominator > 0, 'Degenerate axis')
    scale = numerator/denominator
    origin = measured_mean-scale*logical_mean
    residual = 0.0
    for edge in edges:
        residual = max(residual,abs(edge[5]-origin-scale*edge[0]))
    return origin,scale,residual


def resolver(geometry):
    require(len(geometry) == 5 and all(math.isfinite(value) for value in geometry) and geometry[4] >= 0, 'Geometry numeric contract')
    x,y,sx,sy,residual = geometry
    tolerance = .005/1080
    if not (1-tolerance <= sx <= 2+tolerance and 1-tolerance <= sy <= 2+tolerance):
        return {'firstFailure':'InitialScaleRange','accepted':False,'axes':[]}
    axes = []
    for origin,scale,size in ((x,sx,1920),(y,sy,1080)):
        clamped = min(2.0,max(1.0,scale))
        far = origin+clamped*size
        near_exterior,far_exterior = max(0.0,-origin),max(0.0,far-size)
        near,far_clipped = max(0.0,origin),min(float(size),far)
        valid = near_exterior < .5 and far_exterior < .5 and far_clipped > near and math.isfinite(far)
        axes.append({'inputOrigin':origin,'inputScale':scale,'clampedScale':clamped,'farBeforeClip':far,
            'nearExterior':near_exterior,'farExterior':far_exterior,'snapValid':valid,
            'boundedOrigin':near,'boundedFar':far_clipped,'postSnapScale':(far_clipped-near)/size})
        if not valid:
            return {'firstFailure':'SnapAxisBoundary','accepted':False,'axes':axes}
    for axis,name in ((axes[0],'X'),(axes[1],'Y')):
        if axis['postSnapScale'] < 1:
            return {'firstFailure':'PostSnapScale'+name+'BelowMinimum','accepted':False,'axes':axes}
        if axis['postSnapScale'] > 2:
            return {'firstFailure':'PostSnapScale'+name+'AboveMaximum','accepted':False,'axes':axes}
    return {'firstFailure':None,'accepted':True,'axes':axes}


def same_seed(first,second):
    quantities = [abs(first[0]-second[0]),abs(first[1]-second[1]),1920*abs(first[2]-second[2]),1080*abs(first[3]-second[3])]
    return all(value < 1.5 for value in quantities),quantities


def validate_sample(sample, truth, pixels):
    require(sample['reason'] == 'MeasuredOnly_NoAdmissionCandidate' and sample['originalRefineMatched'] and sample['originalCoverageMatched'], 'Incomplete or divergent geometry observation')
    require(sample['pixelBlake3'] == pixels['pixelBlake3'] and sample['pts'] == pixels['pts'], 'Pixel/time provenance')
    require(sample['finalGeometry'][:4] == truth['geometryFit'], 'Different original runtime geometry')
    require(1 <= len(sample['iterations']) <= 4, 'Refinement iteration budget')
    current = sample['seed']
    summaries = []
    for ordinal,iteration in enumerate(sample['iterations']):
        require(iteration['input'] == current, 'Iteration chain changed')
        edges = iteration['edges']
        require(len(edges) == 48 and all(len(edge) == 12 and all(math.isfinite(value) for value in edge) for edge in edges), '48 finite bounded edge records required')
        sharp = 0
        for index,edge in enumerate(edges):
            logical,predicted,perpendicular,black,white,crossing,left,right,before,after,endpoint_before,endpoint_after = edge
            require(white > black and after != before and right > left, 'Invalid crossing levels')
            midpoint = (black+white)*.5
            reconstructed = right-.5+.5*(midpoint-before)/(after-before)
            require(reconstructed == crossing, 'FindEdge interpolation does not match native crossing')
            rising = (index%6)%2 != 0
            require((before <= midpoint <= after and after > before) if rising else (before >= midpoint >= after and after < before), 'Crossing polarity/bracket changed')
            sharp += int(abs(crossing-round(crossing)) <= 1e-9 and
                abs(endpoint_before-(black if rising else white)) <= (white-black)*1e-9 and
                abs(endpoint_after-(white if rising else black)) <= (white-black)*1e-9)
        ox,sx,rx = ordered_fit(edges[:24])
        oy,sy,ry = ordered_fit(edges[24:])
        require(iteration['fit'] == [ox,oy,sx,sy,max(rx,ry)], 'Ordered OLS differs from native FitAxis')
        movement = max(abs(ox-current[0])+1920*abs(sx-current[2]),abs(oy-current[1])+1080*abs(sy-current[3]))
        require(iteration['movement'] == movement and (ordinal == len(sample['iterations'])-1 or movement > .005), 'Convergence decision changed')
        summaries.append({'iteration':ordinal,'movement':movement,'fit':iteration['fit'],
            'crossingDeltaMin':min(edge[5]-edge[0] for edge in edges),'crossingDeltaMax':max(edge[5]-edge[0] for edge in edges),
            'sharpEndpointSupportedEdges':sharp,'edgeCount':48})
        current = iteration['fit']
    require(current == sample['refined'], 'Final refinement mismatch')
    require(len(sample['iterations']) == 4 or sample['iterations'][-1]['movement'] <= .005, 'Incomplete refinement termination')
    branch = resolver(sample['finalGeometry'])
    require(branch['accepted'] == sample['resolverAccepted'] == truth['bootstrapAccepted'], 'Native resolver/recorded decision differs')
    require(all(0 <= value <= 24000000 for value in sample['work'].values()) and sample['work']['locate']+sample['work']['reference'] <= 24000000, 'Original per-reader budget')
    return {'kind':sample['kind'],'ordinal':sample['ordinal'],'seed':sample['seed'],'finalGeometry':sample['finalGeometry'],
        'iterations':summaries,'resolver':branch,'matchesOriginalRuntime':True}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root',type=local,required=True)
    parser.add_argument('--output',type=local,required=True)
    args = parser.parse_args()
    root,old = args.root,BASE/'artifacts/remote-step3b-20260908-run01'
    require(not args.output.exists(), 'Analysis output create-only')
    first = rows(root/'run-01/samples.jsonl',6)
    supplement = rows(root/'ambiguity-01/samples.jsonl',2)
    require(len(first) == 6 and len(supplement) == 2 and first[-1]['reason'] == 'NotOneMarkerPerRole_StopNoCandidateSelection', 'Preserved initial failure differs')
    require(first[-1]['roleCounts'] == [1,1,1,2] and not first[-1]['iterations'], 'No fabricated observations for the stopped frame')
    require([sample['candidateOrdinal'] for sample in supplement] == [0,1], 'Candidate ordering changed')
    p1,p2 = read_json(root/'logs/probe-01.process.json'),read_json(root/'logs/probe-ambiguity-01.process.json')
    require(p1['returnCode'] == p2['returnCode'] == 1 and not p1['failures'] and not p2['failures'], 'Historical native failures must remain failures')
    require(read_bytes(root/'logs/probe-01.log',MIB).strip() == b'Geometry probe stopped: NotOneMarkerPerRole_StopNoCandidateSelection', 'First failure identity')
    require(read_bytes(root/'logs/probe-ambiguity-01.log',MIB).strip() == b'Candidate and original decision differ', 'Supplement failure identity')
    native_sources = [read_json(root/'context/probe-runtime-start.json'),read_json(root/'context/probe-ambiguity-runtime-start.json')]
    for record in native_sources:
        require(identity(Path(record['exe']['path'])) == record['exe'], 'Historical running binary changed')
        for item in record['runtimeFiles']:
            require(identity(Path(item['path'])) == item, 'Historical runtime dependency changed')
    raw_trace,codec_trace = rows(old/'raw-full/frames.jsonl'),rows(old/'codec-v2-01/replay/frames.jsonl')
    raw_pixels,codec_pixels = rows(old/'raw-full/pixels.jsonl'),rows(old/'codec-v2-01/replay/pixels.jsonl')
    for directory in ('run-01','ambiguity-01'):
        media = rows(root/directory/'media-prefix.jsonl',16)
        require(len(media) == 16 and all(row == {'ordinal':ordinal,'pts':codec_pixels[ordinal]['pts'],'pixelBlake3':codec_pixels[ordinal]['pixelBlake3']} for ordinal,row in enumerate(media)), 'Media decoder provenance differs')
    results = []
    for sample in first[:5]:
        ordinal = sample['ordinal']; raw = sample['kind']=='raw'
        results.append(validate_sample(sample,(raw_trace if raw else codec_trace)[ordinal],(raw_pixels if raw else codec_pixels)[ordinal]))
    require(first[-1]['pixelBlake3'] == codec_pixels[15]['pixelBlake3'], 'Initial stopped sample provenance')
    duplicate,differences = same_seed(supplement[0]['seed'],supplement[1]['seed'])
    require(duplicate and all(row['bootstrapEvaluation']['erasure'] == 0 for row in supplement) and
        supplement[0]['bootstrapEvaluation']['canonicalBlake3'] == supplement[1]['bootstrapEvaluation']['canonicalBlake3'], 'Independent candidate facts changed')
    require(supplement[1]['finalGeometry'] != supplement[0]['finalGeometry'], 'Different independent final fits expected')
    results.append(validate_sample(supplement[0],codec_trace[15],codec_pixels[15]))
    require(read_json(root/'SEED_GUARDS.json')['passed'], 'Native original SameGeometry numeric guards required')
    # The actual production decision follows source-order seed deduplication.
    # The second independent evaluation is a diagnostic counterfactual only.
    branches = []
    for directory in ('codec-v2-01','codec-v2-02'):
        for row in rows(old/directory/'replay/frames.jsonl'):
            # Historical trace lacks residual; its finite/nonnegative bound follows
            # from completed RefineGeometry and Bootstrap stages, not a fake reading.
            result = resolver([*row['geometryFit'],0])
            require(result['firstFailure'] == 'PostSnapScaleXBelowMinimum' and row['bootstrapErasure'] == 12, 'Historical branch reconstruction differs')
            branches.append({'trial':directory,'ordinal':row['observation']-1,'firstFailure':result['firstFailure'],
                'postSnapScaleX':result['axes'][0]['postSnapScale'],'residualSubstitutionIsAlgebraicOnly':True})
    checks = []
    def rejected(label, operation):
        try:
            operation()
        except (ValueError,KeyError,TypeError):
            checks.append(label)
            return
        raise ValueError('Corrupt evidence accepted: '+label)
    bad = copy.deepcopy(first[3]); bad['iterations'][0]['edges'][0][5] += .01
    rejected('crossing-tamper',lambda:validate_sample(bad,codec_trace[0],codec_pixels[0]))
    bad_fit = copy.deepcopy(first[3]); bad_fit['iterations'][0]['fit'][0] += .01
    rejected('fit-tamper',lambda:validate_sample(bad_fit,codec_trace[0],codec_pixels[0]))
    wrong_pixel = copy.deepcopy(first[3]); wrong_pixel['pixelBlake3'] = '0'*64
    rejected('pixel-tamper',lambda:validate_sample(wrong_pixel,codec_trace[0],codec_pixels[0]))
    wrong_native = copy.deepcopy(first[3]); wrong_native['resolverAccepted'] = True
    rejected('native-result-tamper',lambda:validate_sample(wrong_native,codec_trace[0],codec_pixels[0]))
    rejected('deduplicated-second-not-original',lambda:validate_sample(supplement[1],codec_trace[15],codec_pixels[15]))
    records = [read_json(path) for path in (root/'logs').glob('*.process.json')]
    spent = sum(row['processingSeconds'] for row in records)
    require(spent <= 120, 'Total process time budget')
    output = {'schema':'PixelBridge.GeometryCodec.Attribution.1','status':'ATTRIBUTION_COMPLETE_CRITERION_NOT_ESTABLISHED',
        'admissionCandidateImplemented':False,'productFixImplemented':False,'fieldStatus':'NOT_RUN','step3Status':'PARTIAL',
        'nativeRunStatuses':{'initial':'FAILED_STOP_ON_ADDITIONAL_MARKER','supplement':'FAILED_TOOL_EXPECTATION_IGNORED_PRODUCTION_SEED_DEDUP'},
        'bothFailuresPreserved':True,'uniquePixelSamples':6,'pixelSampleAttempts':7,'totalMediaFramesDecoded':32,
        'newWarpObservations':0,'newPayloadFecOrReceiverCalls':0,'initialNewBootstrapFecCalls':0,
        'supplementBootstrapCandidateEvaluationUpperBound':4,'supplementBootstrapRsCopyUpperBound':8,
        'sampleAttribution':results,'allSixSelectedFinalFitsMatchHistoricalRuntime':True,
        'firstGeometricDivergence':{'codec0And1':'Seed remains canonical; measured midpoint crossings change FitAxis result',
            'codec15':'LocateMarkers yields two bottom-right centers; ordered SameGeometry dedup retains first seed before refinement'},
        'seedDedup':{'originalThreshold':1.5,'comparisonQuantities':differences,'duplicate':True,'productionEvaluatedCandidateOrdinal':0,
            'independentSecondEvaluationIsCounterfactual':True,'notAProductionAmbiguousAdmission':True},
        'sixtyHistoricalCodecBranchReconstruction':branches,'historicalResidualNotRecorded':True,
        'selectedNativeResidualsNowMeasured':True,'analysisGuardsPassed':checks,'seedNumericGuards':read_json(root/'SEED_GUARDS.json'),
        'supervisedSecondsIncludingAllBuilds':spent,'nativePeakCommitBytes':max(p1['peakJobCommitBytes'],p2['peakJobCommitBytes']),
        'criterionConclusion':'No observation-derived error bound for lossy edges; cannot promote snapping, threshold changes, or seed fallback to safe admission from these development samples',
        'remainingLimitations':['No new successful full tool run after seed-guard correction','Final build has numeric guard evidence only, not re-observed pixels',
            'No payload recovery or gain measured','No complete loss model or independent holdout'],
        'nextGate':'Submit bounded measurement-model/estimator design for approval; do not implement product admission changes'}
    write_json(args.output,output)
    print(json.dumps({key:output[key] for key in ('status','uniquePixelSamples','pixelSampleAttempts','totalMediaFramesDecoded','supervisedSecondsIncludingAllBuilds')},indent=2))


if __name__ == '__main__':
    main()
