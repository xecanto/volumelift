// Lifecycle log for the APO, written to the audio engine's temp directory
// (C:\Windows\ServiceProfiles\LocalService\AppData\Local\Temp\VolumeLiftApo.log).
// Never call from APOProcess: it does file I/O.
#pragma once

namespace vl {

void ApoLog(const wchar_t* format, ...);

}  // namespace vl
