PixelBridge RemoteVisual LF4 - Computer B single-monitor fullscreen sender
===========================================================================

Contents required in the delivery folder:

  Start-PBRemoteVisualSingleMonitorFullscreen.bat
  random-1MiB.bin
  RuntimePackage\PixelBridgeEncoder.exe
  RuntimePackage\*.dll and RuntimePackage\plugin subdirectories

Operation on Computer B:

1. Copy and extract the entire delivery folder. Do not copy only the BAT file.
2. Close the PixelBridge Encoder GUI that was previously opened.
3. Double-click Start-PBRemoteVisualSingleMonitorFullscreen.bat.
   If the delivery contains shared-run-id.txt, the launcher consumes that
   one-run identity so Computer A and B reports can be joined. Later launches
   generate a fresh OS-CSPRNG RunId automatically. A pre-existing runs\RunId
   directory is rejected; the launcher never overwrites earlier evidence.
4. The primary physical monitor is covered by a borderless topmost LF4 raster.
   The outer fullscreen surface uses the monitor's exact physical size. The
   logical 1920x1080 LF4 canvas remains exact 1:1 and is centered on a neutral
   background, so a 2560x1600 monitor remains a genuine 16:10 outer picture.
   Any later remote-desktop/video scaling is solved from captured locator pixels.
5. Do not minimize, cover, rotate, or change the display mode during transfer.
6. Wait until the Computer A Decoder reports WholeFileDigest verification and
   successful final publish. Then press Q or Enter once. Do not stop merely
   because the sender has completed one carousel cycle; it loops continuously.

Default source truth:

  file: random-1MiB.bin
  bytes: 1048576
  SHA-256: 93f85aa63ed348ef4d565cd0bb942b2417ba4bb6e5ffafd223239ddfd83d3587

Truth boundary:

This launcher uses the production LF4 SenderFrameBuilder, FEC, Transport and
carousel path. It is an explicit user-authorized single-monitor presentation
mode, so it does not claim the formal dual-monitor ProtectedMonitor field Gate.
The receiver's file acceptance remains unchanged: Receiver, WholeFileDigest,
safe publish and an external SHA-256 comparison are still required.
