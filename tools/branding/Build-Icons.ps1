param([string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
foreach ($role in @('Encoder', 'Decoder'))
{
    $directory = Join-Path $RepositoryRoot "apps/PixelBridge$role/resources"
    $source = [System.Drawing.Image]::FromFile((Join-Path $directory 'logo.png'))
    try
    {
        $frames = @()
        foreach ($size in @(16, 24, 32, 48, 64, 128, 256))
        {
            $bitmap = New-Object System.Drawing.Bitmap($size, $size)
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            $stream = New-Object System.IO.MemoryStream
            try
            {
                $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $graphics.DrawImage($source, 0, 0, $size, $size)
                $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
                $frames += [pscustomobject]@{ Size = $size; Bytes = $stream.ToArray() }
            }
            finally { $stream.Dispose(); $graphics.Dispose(); $bitmap.Dispose() }
        }
        $output = [System.IO.File]::Create((Join-Path $directory 'application.ico'))
        $writer = New-Object System.IO.BinaryWriter($output)
        try
        {
            $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$frames.Count)
            $offset = 6 + 16 * $frames.Count
            foreach ($frame in $frames)
            {
                $dimension = if ($frame.Size -eq 256) { 0 } else { $frame.Size }
                $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
                $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]32)
                $writer.Write([uint32]$frame.Bytes.Length); $writer.Write([uint32]$offset)
                $offset += $frame.Bytes.Length
            }
            foreach ($frame in $frames) { $writer.Write([byte[]]$frame.Bytes) }
        }
        finally { $writer.Dispose(); $output.Dispose() }
    }
    finally { $source.Dispose() }
}
