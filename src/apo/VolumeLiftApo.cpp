#include "apo/VolumeLiftApo.h"

#include "common/Config.h"

#include <cstring>
#include <new>

#ifndef APOERR_FORMAT_NOT_SUPPORTED
#define APOERR_FORMAT_NOT_SUPPORTED MAKE_HRESULT(SEVERITY_ERROR, FACILITY_AUDIO, 0x0003)
#endif
#ifndef APOERR_ALREADY_LOCKED
#define APOERR_ALREADY_LOCKED MAKE_HRESULT(SEVERITY_ERROR, FACILITY_AUDIO, 0x0002)
#endif
#ifndef APOERR_NOT_LOCKED
#define APOERR_NOT_LOCKED MAKE_HRESULT(SEVERITY_ERROR, FACILITY_AUDIO, 0x0004)
#endif

namespace vl {

std::atomic<long> g_serverRefs{0};

namespace {

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, defined here to avoid pulling in ksmedia GUID storage.
constexpr GUID kFloatSubtype = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

bool IsFloat32(const UNCOMPRESSEDAUDIOFORMAT& format) {
    return IsEqualGUID(format.guidFormatType, kFloatSubtype) && format.dwBytesPerSampleContainer == 4 &&
           format.dwSamplesPerFrame > 0;
}

// The APO processes in place, so input and output must be the same 32-bit float layout.
HRESULT CheckFormat(IAudioMediaType* opposite, IAudioMediaType* requested, IAudioMediaType** supported) {
    if (!requested || !supported) {
        return E_POINTER;
    }
    *supported = nullptr;

    UNCOMPRESSEDAUDIOFORMAT format{};
    if (FAILED(requested->GetUncompressedAudioFormat(&format)) || !IsFloat32(format)) {
        return APOERR_FORMAT_NOT_SUPPORTED;
    }
    if (opposite) {
        UNCOMPRESSEDAUDIOFORMAT other{};
        if (SUCCEEDED(opposite->GetUncompressedAudioFormat(&other)) &&
            (other.dwSamplesPerFrame != format.dwSamplesPerFrame || other.fFramesPerSecond != format.fFramesPerSecond)) {
            return APOERR_FORMAT_NOT_SUPPORTED;
        }
    }
    requested->AddRef();
    *supported = requested;
    return S_OK;
}

}  // namespace

VolumeLiftApo::VolumeLiftApo() { ++g_serverRefs; }

VolumeLiftApo::~VolumeLiftApo() {
    settings_.Stop();
    --g_serverRefs;
}

STDMETHODIMP VolumeLiftApo::QueryInterface(REFIID riid, void** object) {
    if (!object) {
        return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioProcessingObject)) {
        *object = static_cast<IAudioProcessingObject*>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectRT)) {
        *object = static_cast<IAudioProcessingObjectRT*>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectConfiguration)) {
        *object = static_cast<IAudioProcessingObjectConfiguration*>(this);
    } else if (riid == __uuidof(IAudioSystemEffects)) {
        *object = static_cast<IAudioSystemEffects*>(this);
    } else {
        *object = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) VolumeLiftApo::AddRef() { return ++refs_; }

STDMETHODIMP_(ULONG) VolumeLiftApo::Release() {
    const ULONG refs = --refs_;
    if (refs == 0) {
        delete this;
    }
    return refs;
}

STDMETHODIMP VolumeLiftApo::Reset() {
    booster_.Reset();
    return S_OK;
}

STDMETHODIMP VolumeLiftApo::GetLatency(HNSTIME* time) {
    if (!time) {
        return E_POINTER;
    }
    *time = 0;
    return S_OK;
}

void VolumeLiftApo::FillRegistration(APO_REG_PROPERTIES& props) {
    props = {};
    props.clsid = kApoClsid;
    props.Flags = static_cast<APO_FLAG>(APO_FLAG_INPLACE | APO_FLAG_FRAMESPERSECOND_MUST_MATCH |
                                        APO_FLAG_BITSPERSAMPLE_MUST_MATCH | APO_FLAG_SAMPLESPERFRAME_MUST_MATCH);
    wcscpy_s(props.szFriendlyName, kApoFriendlyName);
    wcscpy_s(props.szCopyrightInfo, L"VolumeLift contributors, MIT License");
    props.u32MajorVersion = 1;
    props.u32MinorVersion = 0;
    props.u32MinInputConnections = 1;
    props.u32MaxInputConnections = 1;
    props.u32MinOutputConnections = 1;
    props.u32MaxOutputConnections = 1;
    props.u32MaxInstances = 0xffffffff;
    props.u32NumAPOInterfaces = 1;
    props.iidAPOInterfaceList[0] = __uuidof(IAudioProcessingObject);
}

STDMETHODIMP VolumeLiftApo::GetRegistrationProperties(APO_REG_PROPERTIES** properties) {
    if (!properties) {
        return E_POINTER;
    }
    auto* props = static_cast<APO_REG_PROPERTIES*>(CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES)));
    if (!props) {
        return E_OUTOFMEMORY;
    }
    FillRegistration(*props);
    *properties = props;
    return S_OK;
}

STDMETHODIMP VolumeLiftApo::Initialize(UINT32 /*cbDataSize*/, BYTE* /*pbyData*/) {
    // System effects receive endpoint details here; the boost does not need them.
    return S_OK;
}

STDMETHODIMP VolumeLiftApo::IsInputFormatSupported(IAudioMediaType* opposite, IAudioMediaType* requested,
                                                   IAudioMediaType** supported) {
    return CheckFormat(opposite, requested, supported);
}

STDMETHODIMP VolumeLiftApo::IsOutputFormatSupported(IAudioMediaType* opposite, IAudioMediaType* requested,
                                                    IAudioMediaType** supported) {
    return CheckFormat(opposite, requested, supported);
}

STDMETHODIMP VolumeLiftApo::GetInputChannelCount(UINT32* count) {
    if (!count) {
        return E_POINTER;
    }
    *count = channels_;
    return S_OK;
}

STDMETHODIMP VolumeLiftApo::LockForProcess(UINT32 inputCount, APO_CONNECTION_DESCRIPTOR** inputs, UINT32 outputCount,
                                           APO_CONNECTION_DESCRIPTOR** outputs) {
    if (locked_) {
        return APOERR_ALREADY_LOCKED;
    }
    if (inputCount != 1 || outputCount != 1 || !inputs || !outputs || !inputs[0] || !inputs[0]->pFormat) {
        return E_INVALIDARG;
    }
    UNCOMPRESSEDAUDIOFORMAT format{};
    HRESULT hr = inputs[0]->pFormat->GetUncompressedAudioFormat(&format);
    if (FAILED(hr)) {
        return hr;
    }
    if (!IsFloat32(format)) {
        return APOERR_FORMAT_NOT_SUPPORTED;
    }
    channels_ = format.dwSamplesPerFrame;
    booster_.Prepare(format.fFramesPerSecond, channels_);
    booster_.SetTarget(settings_.GainDb(), settings_.Limiter());
    settings_.Start();
    locked_ = true;
    return S_OK;
}

STDMETHODIMP VolumeLiftApo::UnlockForProcess() {
    if (!locked_) {
        return APOERR_NOT_LOCKED;
    }
    settings_.Stop();
    locked_ = false;
    return S_OK;
}

STDMETHODIMP_(void) VolumeLiftApo::APOProcess(UINT32 inputCount, APO_CONNECTION_PROPERTY** inputs,
                                              UINT32 outputCount, APO_CONNECTION_PROPERTY** outputs) {
    if (inputCount < 1 || outputCount < 1) {
        return;
    }
    APO_CONNECTION_PROPERTY* in = inputs[0];
    APO_CONNECTION_PROPERTY* out = outputs[0];
    const UINT32 frames = in->u32ValidFrameCount;

    if (in->u32BufferFlags == BUFFER_SILENT || frames == 0) {
        out->u32ValidFrameCount = frames;
        out->u32BufferFlags = in->u32BufferFlags == BUFFER_INVALID ? BUFFER_INVALID : BUFFER_SILENT;
        return;
    }

    auto* source = reinterpret_cast<float*>(in->pBuffer);
    auto* dest = reinterpret_cast<float*>(out->pBuffer);
    if (dest != source) {
        std::memcpy(dest, source, static_cast<size_t>(frames) * channels_ * sizeof(float));
    }
    if (settings_.Enabled()) {
        booster_.SetTarget(settings_.GainDb(), settings_.Limiter());
    } else {
        booster_.SetTarget(0.0f, false);
    }
    booster_.Process(dest, frames);

    out->u32ValidFrameCount = frames;
    out->u32BufferFlags = BUFFER_VALID;
}

STDMETHODIMP_(UINT32) VolumeLiftApo::CalcInputFrames(UINT32 outputFrameCount) { return outputFrameCount; }

STDMETHODIMP_(UINT32) VolumeLiftApo::CalcOutputFrames(UINT32 inputFrameCount) { return inputFrameCount; }

}  // namespace vl
