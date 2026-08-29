#Requires -Version 7.0

function Get-DesktopLevelsNativeLoopPolicy([string]$NativeMode)
{
    $normalizedMode = $NativeMode.ToLowerInvariant()
    if ($normalizedMode -notin @('release','asan'))
    {
        throw "Unsupported native Gate mode: $NativeMode"
    }

    $captureSeconds = 30
    $receiverDeadlineSeconds = 45
    $senderFrames = 80
    $sequenceIntervalMilliseconds = 640
    $senderLifetimeMilliseconds = [UInt64]$senderFrames * $sequenceIntervalMilliseconds
    $minimumSenderLifetimeMilliseconds = [UInt64]($receiverDeadlineSeconds + 2) * 1000
    if ($senderLifetimeMilliseconds -le $minimumSenderLifetimeMilliseconds)
    {
        throw 'Sender lifetime must exceed the bounded receiver deadline plus startup margin'
    }

    return [pscustomobject]@{
        NativeMode=$normalizedMode
        CaptureSeconds=$captureSeconds
        ReceiverDeadlineSeconds=$receiverDeadlineSeconds
        SenderFrames=$senderFrames
        SequenceIntervalMilliseconds=$sequenceIntervalMilliseconds
        SenderLifetimeMilliseconds=$senderLifetimeMilliseconds
        SenderShutdownMilliseconds=30000
        RequiredVerifiedFrames=if ($normalizedMode -eq 'asan') { 8 } else { 16 }
        RequiredVerifiedPhases=if ($normalizedMode -eq 'asan') { 8 } else { 16 }
    }
}
