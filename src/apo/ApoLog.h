// Lifecycle log for the APO, written to %ProgramData%\VolumeLift\apo.log.
// Never call from APOProcess or DllMain: it does file I/O.
#pragma once

namespace vl {

void ApoLog(const wchar_t* format, ...);

}  // namespace vl
