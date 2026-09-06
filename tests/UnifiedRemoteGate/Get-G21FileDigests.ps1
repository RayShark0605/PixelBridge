param(
    [Parameter(Mandatory = $true)][string]$InputPath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [Parameter(Mandatory = $true)][string]$Blake3Dll,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedDllSha256
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $OutputPath) { throw 'Create-only digest output already exists.' }
$library = (Resolve-Path -LiteralPath $Blake3Dll).ProviderPath
if ((Get-FileHash -LiteralPath $library -Algorithm SHA256).Hash -ine $ExpectedDllSha256) { throw 'Pinned BLAKE3 library hash mismatch.' }
if (-not ('G21IndependentDigestReader' -as [type]))
{
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
public static class G21IndependentDigestReader
{
    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct ChunkLayout
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public uint[] Cv;
        public ulong ChunkCounter;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public byte[] Buffer;
        public byte BufferLength;
        public byte BlocksCompressed;
        public byte Flags;
    }
    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct HasherLayout
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public uint[] Key;
        public ChunkLayout Chunk;
        public byte StackLength;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 1760)] public byte[] Stack;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryExW(string path, IntPtr reserved, uint flags);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);
    [DllImport("kernel32.dll")]
    private static extern bool FreeLibrary(IntPtr module);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr VersionFunction();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void InitFunction(IntPtr state);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void UpdateFunction(IntPtr state, byte[] data, UIntPtr length);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void FinalizeFunction(IntPtr state, byte[] output, UIntPtr length);
    private static Delegate Function(IntPtr module, string name, Type type)
    {
        IntPtr address = GetProcAddress(module, name);
        if (address == IntPtr.Zero) { throw new InvalidOperationException("Missing BLAKE3 export: " + name); }
        return Marshal.GetDelegateForFunctionPointer(address, type);
    }
    public static string[] Read(string file, string library)
    {
        int stateBytes = Marshal.SizeOf(typeof(HasherLayout));
        if (IntPtr.Size != 8 || stateBytes != 1912) { throw new InvalidOperationException("Unsupported BLAKE3 1.8.5 x64 ABI."); }
        IntPtr module = LoadLibraryExW(library, IntPtr.Zero, 0x1100);
        if (module == IntPtr.Zero) { throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()); }
        IntPtr state = IntPtr.Zero;
        try
        {
            var version = (VersionFunction)Function(module, "blake3_version", typeof(VersionFunction));
            if (Marshal.PtrToStringAnsi(version()) != "1.8.5") { throw new InvalidOperationException("Unexpected pinned BLAKE3 version."); }
            var initialize = (InitFunction)Function(module, "blake3_hasher_init", typeof(InitFunction));
            var update = (UpdateFunction)Function(module, "blake3_hasher_update", typeof(UpdateFunction));
            var finalize = (FinalizeFunction)Function(module, "blake3_hasher_finalize", typeof(FinalizeFunction));
            state = Marshal.AllocHGlobal(stateBytes);
            Marshal.Copy(new byte[stateBytes], 0, state, stateBytes);
            initialize(state);
            byte[] buffer = new byte[1024 * 1024];
            byte[] digest = new byte[32];
            long total = 0;
            using (var input = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read, buffer.Length, FileOptions.SequentialScan))
            using (var sha256 = SHA256.Create())
            {
                int count;
                while ((count = input.Read(buffer, 0, buffer.Length)) != 0)
                {
                    checked { total += count; }
                    sha256.TransformBlock(buffer, 0, count, buffer, 0);
                    update(state, buffer, new UIntPtr((uint)count));
                }
                sha256.TransformFinalBlock(buffer, 0, 0);
                finalize(state, digest, new UIntPtr(32));
                if (total != input.Length) { throw new IOException("Exact byte count changed during locked audit."); }
                return new[] { total.ToString(CultureInfo.InvariantCulture), BitConverter.ToString(sha256.Hash).Replace("-", "").ToLowerInvariant(), BitConverter.ToString(digest).Replace("-", "").ToLowerInvariant() };
            }
        }
        finally
        {
            if (state != IntPtr.Zero) { Marshal.FreeHGlobal(state); }
            FreeLibrary(module);
        }
    }
}
'@
}
$resolved = (Resolve-Path -LiteralPath $InputPath).ProviderPath
$values = [G21IndependentDigestReader]::Read($resolved, $library)
$result = [ordered]@{
    schema = 'PixelBridge.G21.IndependentFileDigests.1'
    fileName = [IO.Path]::GetFileName($resolved)
    bytes = [long]::Parse($values[0], [Globalization.CultureInfo]::InvariantCulture)
    sha256 = $values[1]
    blake3 = $values[2]
    implementation = 'Independent post-stop PowerShell process; SHA256 .NET; pinned BLAKE3 1.8.5 C API; locked read; bounded 1 MiB buffer'
    librarySha256 = $ExpectedDllSha256.ToLowerInvariant()
    completedUtc = [DateTime]::UtcNow.ToString('o')
}
$encoded = [Text.UTF8Encoding]::new($false).GetBytes(($result | ConvertTo-Json -Depth 4))
$output = [IO.File]::Open($OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
try { $output.Write($encoded, 0, $encoded.Length); $output.Flush($true) } finally { $output.Dispose() }
Write-Host ('PASS: independent file digests; ' + $result.bytes + ' bytes; ' + $result.fileName)
