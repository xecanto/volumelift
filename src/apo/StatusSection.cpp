#include "apo/StatusSection.h"

#include <sddl.h>

namespace vl {

StatusSection::~StatusSection() {
    if (view_) {
        UnmapViewOfFile(view_);
    }
    if (mapping_) {
        CloseHandle(mapping_);
    }
}

void StatusSection::Open() {
    if (view_) {
        return;
    }
    // Writable by the audio engine's account and SYSTEM; readable by everyone, including app containers.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GR;;;WD)(A;;GR;;;AC)",
                                                              SDDL_REVISION_1, &descriptor, nullptr)) {
        return;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0, sizeof(EngineStatus),
                                  kStatusSectionName);
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    LocalFree(descriptor);
    if (!mapping_) {
        return;
    }
    view_ = static_cast<EngineStatus*>(MapViewOfFile(mapping_, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0,
                                                     sizeof(EngineStatus)));
    if (view_ && !existed) {
        view_->version = kStatusVersion;
        view_->limiterPermille = 1000;
        view_->magic = kStatusMagic;
    }
}

}  // namespace vl
