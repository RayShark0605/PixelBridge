using System;
using System.Diagnostics;
using System.IO;
using System.Threading;

// Process-control fixture only. No capture, raster, receiver, codec or payload transport.
public static class WrapperLifecycleFixture
{
    public static int Main(string[] arguments)
    {
        string executable = Process.GetCurrentProcess().MainModule.FileName;
        string directory = Path.GetDirectoryName(executable);
        string mode = File.ReadAllText(Path.Combine(directory, "fixture-mode.txt")).Trim();
        if (arguments.Length == 1 && arguments[0] == "--build-identity")
        {
            Console.WriteLine("{\"gitCommit\":\"g21-lifecycle-fixture\",\"applicationName\":\"" + Path.GetFileNameWithoutExtension(executable) + "\",\"fixtureOnly\":true}");
            return 0;
        }
        if (arguments.Length == 1 && arguments[0] == "--self-test") { return 0; }
        if (arguments.Length == 2 && arguments[0] == "--preflight")
        {
            Directory.CreateDirectory(arguments[1]);
            File.WriteAllText(Path.Combine(arguments[1], "fixture-only.json"), "{\"fixtureOnly\":true,\"monitorEnumerationPerformed\":false}");
            return mode == "preflight-failure" ? 5 : 0;
        }
        if (arguments.Length == 3 && (arguments[0] == "--receive" || arguments[0] == "--receive-eventual"))
        {
            Directory.CreateDirectory(arguments[1]);
            File.WriteAllText(Path.Combine(directory, "fixture-pid.txt"), Process.GetCurrentProcess().Id.ToString());
            string sample = Path.Combine(arguments[1], "samples.jsonl");
            bool coverage = mode != "coverage" && mode != "waiting";
            bool available = mode != "waiting";
            bool counterMode = mode.StartsWith("counter:", StringComparison.Ordinal);
            string counter = counterMode ? mode.Substring(8) : "outerResourceRejections";
            long deferredBusyCount = mode == "recoverable-busy" || mode == "recoverable-busy-not-full" ? 1 : 0;
            long fecQuotaCount = deferredBusyCount;
            if (counter == "outerDeferredResourceBusyCount") { deferredBusyCount = 1; }
            if (counter == "outerFecQuotaExceededCount") { fecQuotaCount = 1; }
            long activeDecoderLimit = 8;
            long activeDecoderCount = mode == "recoverable-busy" ? 7 : 0;
            long peakActiveDecoderCount = mode == "recoverable-busy-not-full" ? 7 : 8;
            long totalDecoderByteLimit = 1073741824;
            long reservedDecoderBytes = mode == "recoverable-busy" ? 399876484 : 0;
            long peakReservedDecoderBytes = 457201696;
            string report = "{\"fixtureOnly\":true,\"unifiedTelemetry\":{\"observationAvailable\":" + available.ToString().ToLowerInvariant() +
                ",\"frameCoverageComplete\":" + coverage.ToString().ToLowerInvariant() + ",\"counterOverflow\":" + (mode == "overflow" ? "true" : "false") +
                "},\"remoteGate\":{\"outerResourceRejections\":" + (counterMode && counter == "outerResourceRejections" ? "1" : "0") +
                ",\"outerConflictRejections\":" + (counterMode && counter == "outerConflictRejections" ? "1" : "0") +
                ",\"receiverResourcePolicyRejectedCount\":" + (counterMode && counter == "receiverResourcePolicyRejectedCount" ? "1" : "0") +
                ",\"receiverControlRejectedByResourcePolicyCount\":" + (counterMode && counter == "receiverControlRejectedByResourcePolicyCount" ? "1" : "0") +
                ",\"outerOrphanDroppedByQuotaCount\":" + (counterMode && counter == "outerOrphanDroppedByQuotaCount" ? "1" : "0") +
                ",\"outerOrphanResourceExhaustedCount\":" + (counterMode && counter == "outerOrphanResourceExhaustedCount" ? "1" : "0") +
                ",\"outerOrphanConflictRejectionCount\":" + (counterMode && counter == "outerOrphanConflictRejectionCount" ? "1" : "0") +
                ",\"outerDeferredResourceBusyCount\":" + deferredBusyCount + ",\"outerFecQuotaExceededCount\":" + fecQuotaCount +
                ",\"outerActiveDecoderLimit\":" + activeDecoderLimit + ",\"outerTotalDecoderByteLimit\":" + totalDecoderByteLimit +
                ",\"outerActiveDecoderCount\":" + activeDecoderCount + ",\"outerPeakActiveDecoderCount\":" + peakActiveDecoderCount +
                ",\"outerReservedDecoderBytes\":" + reservedDecoderBytes + ",\"outerPeakReservedDecoderBytes\":" + peakReservedDecoderBytes + "}}\n";
            if (mode == "partial")
            {
                File.WriteAllText(sample, "{\"fixtureOnly\":");
                Thread.Sleep(1500);
            }
            File.WriteAllText(sample, report);
            bool failure = mode == "coverage" || mode == "overflow" || counterMode || mode == "recoverable-busy-not-full";
            Thread.Sleep(failure ? 30000 : 2300);
            return mode == "exit-seven" ? 7 : 0;
        }
        if (arguments.Length > 0 && arguments[0] == "--headless-broadcast")
        {
            int sourceIndex = Array.IndexOf(arguments, "--source");
            bool writeDenied = false;
            try
            {
                using (var source = new FileStream(arguments[sourceIndex + 1], FileMode.Open, FileAccess.Write, FileShare.ReadWrite)) { }
            }
            catch (IOException) { writeDenied = true; }
            File.WriteAllText(Path.Combine(directory, "fixture-source-lease.json"), "{\"fixtureOnly\":true,\"writeDenied\":" + writeDenied.ToString().ToLowerInvariant() + "}");
            return mode == "exit-seven" ? 7 : 0;
        }
        return 9;
    }
}
