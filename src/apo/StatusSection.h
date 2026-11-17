// Creates (or opens) the shared EngineStatus section from inside the audio engine.
#pragma once

#include "common/EngineStatus.h"

namespace vl {

class StatusSection {
public:
    StatusSection() = default;
    ~StatusSection();
    StatusSection(const StatusSection&) = delete;
    StatusSection& operator=(const StatusSection&) = delete;

    // Not real-time safe; call from LockForProcess or another non-audio thread.
    void Open();
    // Null if the section could not be created; callers must check.
    EngineStatus* Get() const { return view_; }

private:
    HANDLE mapping_ = nullptr;
    EngineStatus* view_ = nullptr;
};

}  // namespace vl
