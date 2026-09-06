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
        if (arguments.Length == 3 && arguments[0] == "--receive")
        {
            Directory.CreateDirectory(arguments[1]);
            File.WriteAllText(Path.Combine(directory, "fixture-pid.txt"), Process.GetCurrentProcess().Id.ToString());
            string sample = Path.Combine(arguments[1], "samples.jsonl");
            bool coverage = mode != "coverage" && mode != "waiting";
            bool available = mode != "waiting";
            string counter = mode.StartsWith("counter:", StringComparison.Ordinal) ? mode.Substring(8) : "outerResourceRejections";
            string report = "{\"fixtureOnly\":true,\"unifiedTelemetry\":{\"observationAvailable\":" + available.ToString().ToLowerInvariant() +
                ",\"frameCoverageComplete\":" + coverage.ToString().ToLowerInvariant() + ",\"counterOverflow\":" + (mode == "overflow" ? "true" : "false") +
                "},\"remoteGate\":{\"" + counter + "\":" + (mode.StartsWith("counter:", StringComparison.Ordinal) ? "1" : "0") + "}}\n";
            if (mode == "partial")
            {
                File.WriteAllText(sample, "{\"fixtureOnly\":");
                Thread.Sleep(1500);
            }
            File.WriteAllText(sample, report);
            bool failure = mode == "coverage" || mode == "overflow" || mode.StartsWith("counter:", StringComparison.Ordinal);
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
