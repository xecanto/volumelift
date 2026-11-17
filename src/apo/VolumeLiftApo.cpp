#include "apo/VolumeLiftApo.h"

#include "apo/ApoLog.h"
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

void LogFormat(const wchar_t* label, IAudioMediaType* type) {
    UNCOMPRESSEDAUDIOFORMAT f{};
    if (!type) {
        ApoLog(L"  %s: (null)", label);
    } else if (FAILED(type->GetUncompressedAudioFormat(&f))) {
        ApoLog(L"  %s: not uncompressed", label);
    } else {
        ApoLog(L"  %s: type %08lX ch=%lu container=%lu valid=%lu rate=%.0f mask=%lX", label, f.guidFormatType.Data1,
               f.dwSamplesPerFrame, f.dwBytesPerSampleContainer, f.dwValidBitsPerSample, f.fFramesPerSecond,
               f.dwChannelMask);
    }
}

// The APO processes in place, so input and output must be the same 32-bit float layout.
HRESULT CheckFormat(const wchar_t* side, IAudioMediaType* opposite, IAudioMediaType* requested,
                    IAudioMediaType** supported) {
    ApoLog(L"Is%sFormatSupported", side);
    LogFormat(L"requested", requested);
    LogFormat(L"opposite", opposite);
    if (!requested || !supported) {
        return E_POINTER;
    }
    *supported = nullptr;

    HRESULT result = S_OK;
    UNCOMPRESSEDAUDIOFORMAT format{};
    if (FAILED(requested->GetUncompressedAudioFormat(&format)) || !IsFloat32(format)) {
        result = APOERR_FORMAT_NOT_SUPPORTED;
    } else if (opposite) {
        UNCOMPRESSEDAUDIOFORMAT other{};
        if (SUCCEEDED(opposite->GetUncompressedAudioFormat(&other)) &&
            (other.dwSamplesPerFrame != format.dwSamplesPerFrame || other.fFramesPerSecond != format.fFramesPerSecond)) {
            result = APOERR_FORMAT_NOT_SUPPORTED;
        }
    }
    if (SUCCEEDED(result)) {
        requested->AddRef();
        *supported = requested;
    }
    ApoLog(L"  -> 0x%08lX", static_cast<unsigned long>(result));
    return result;
}

}  // namespace

VolumeLiftApo::VolumeLiftApo(IUnknown* outer) : outer_(outer ? outer : &inner_) {
    ++g_serverRefs;
    ApoLog(L"APO %p created%s", this, outer ? L" (aggregated)" : L"");
}

VolumeLiftApo::~VolumeLiftApo() {
    settings_.Stop();
    if (locked_ && status_.Get()) {
        InterlockedDecrement(&status_.Get()->lockedInstances);
    }
    --g_serverRefs;
    ApoLog(L"APO %p destroyed", this);
}

STDMETHODIMP VolumeLiftApo::InnerUnknown::QueryInterface(REFIID riid, void** object) {
    if (!object) {
        return E_POINTER;
    }
    IUnknown* found = nullptr;
    if (riid == __uuidof(IUnknown)) {
        found = this;
    } else if (riid == __uuidof(IAudioProcessingObject)) {
        found = static_cast<IAudioProcessingObject*>(owner_);
    } else if (riid == __uuidof(IAudioProcessingObjectRT)) {
        found = static_cast<IAudioProcessingObjectRT*>(owner_);
    } else if (riid == __uuidof(IAudioProcessingObjectConfiguration)) {
        found = static_cast<IAudioProcessingObjectConfiguration*>(owner_);
    } else if (riid == __uuidof(IAudioSystemEffects)) {
        found = static_cast<IAudioSystemEffects*>(owner_);
    }
    if (!found) {
        *object = nullptr;
        wchar_t iid[64] = {};
        StringFromGUID2(riid, iid, 64);
        ApoLog(L"APO %p QueryInterface %s -> E_NOINTERFACE", owner_, iid);
        return E_NOINTERFACE;
    }
    found->AddRef();  // delegates to the controlling unknown, except for the inner IUnknown itself
    *object = found;
    return S_OK;
}

STDMETHODIMP_(ULONG) VolumeLiftApo::InnerUnknown::AddRef() { return ++refs_; }

STDMETHODIMP_(ULONG) VolumeLiftApo::InnerUnknown::Release() {
    const ULONG refs = --refs_;
    if (refs == 0) {
        delete owner_;
    }
    return refs;
}

STDMETHODIMP VolumeLiftApo::QueryInterface(REFIID riid, void** object) { return outer_->QueryInterface(riid, object); }

STDMETHODIMP_(ULONG) VolumeLiftApo::AddRef() { return outer_->AddRef(); }

STDMETHODIMP_(ULONG) VolumeLiftApo::Release() { return outer_->Release(); }

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

STDMETHODIMP VolumeLiftApo::Initialize(UINT32 cbDataSize, BYTE* /*pbyData*/) {
    // System effects receive endpoint details here; the boost does not need them.
    ApoLog(L"APO %p Initialize(%u bytes)", this, cbDataSize);
    return S_OK;
}

STDMETHODIMP VolumeLiftApo::IsInputFormatSupported(IAudioMediaType* opposite, IAudioMediaType* requested,
                                                   IAudioMediaType** supported) {
    return CheckFormat(L"Input", opposite, requested, supported);
}

STDMETHODIMP VolumeLiftApo::IsOutputFormatSupported(IAudioMediaType* opposite, IAudioMediaType* requested,
                                                    IAudioMediaType** supported) {
    return CheckFormat(L"Output", opposite, requested, supported);
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
    status_.Open();
    EngineStatus* status = status_.Get();
    if (status) {
        InterlockedIncrement(&status->lockAttempts);
    }
    ApoLog(L"APO %p LockForProcess(in=%u, out=%u) status=%p", this, inputCount, outputCount, status);
    if (inputCount > 0 && inputs && inputs[0]) {
        LogFormat(L"lock input", inputs[0]->pFormat);
    }
    const auto fail = [this, status](HRESULT hr) {
        ApoLog(L"APO %p LockForProcess failed 0x%08lX", this, static_cast<unsigned long>(hr));
        if (status) {
            status->lastLockResult = hr;
            InterlockedIncrement(&status->lockFailures);
        }
        return hr;
    };

    if (inputCount != 1 || outputCount != 1 || !inputs || !outputs || !inputs[0] || !inputs[0]->pFormat) {
        return fail(E_INVALIDARG);
    }
    UNCOMPRESSEDAUDIOFORMAT format{};
    HRESULT hr = inputs[0]->pFormat->GetUncompressedAudioFormat(&format);
    if (FAILED(hr)) {
        return fail(hr);
    }
    if (status) {
        status->channels = static_cast<LONG>(format.dwSamplesPerFrame);
        status->sampleRate = static_cast<LONG>(format.fFramesPerSecond);
    }
    if (!IsFloat32(format)) {
        return fail(APOERR_FORMAT_NOT_SUPPORTED);
    }
    channels_ = format.dwSamplesPerFrame;
    booster_.Prepare(format.fFramesPerSecond, channels_);
    booster_.SetTarget(settings_.GainDb(), settings_.Limiter());
    settings_.Start(status);
    locked_ = true;
    if (status) {
        status->lastLockResult = S_OK;
        InterlockedIncrement(&status->lockedInstances);
    }
    ApoLog(L"APO %p locked: %u ch @ %.0f Hz, gain %.1f dB", this, channels_, format.fFramesPerSecond,
           settings_.GainDb());
    return S_OK;
}

STDMETHODIMP VolumeLiftApo::UnlockForProcess() {
    if (!locked_) {
        return APOERR_NOT_LOCKED;
    }
    settings_.Stop();
    locked_ = false;
    if (status_.Get()) {
        InterlockedDecrement(&status_.Get()->lockedInstances);
    }
    ApoLog(L"APO %p unlocked", this);
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
    const bool enabled = settings_.Enabled();
    const float gainDb = enabled ? settings_.GainDb() : 0.0f;
    booster_.SetTarget(gainDb, enabled && settings_.Limiter());
    booster_.Process(dest, frames);

    out->u32ValidFrameCount = frames;
    out->u32BufferFlags = BUFFER_VALID;

    if (EngineStatus* status = status_.Get()) {
        float peak = 0.0f;
        const size_t samples = static_cast<size_t>(frames) * channels_;
        for (size_t i = 0; i < samples; ++i) {
            peak = std::max(peak, std::fabs(dest[i]));
        }
        status->peakPermille = static_cast<LONG>(peak * 1000.0f);
        status->limiterPermille = static_cast<LONG>(booster_.LimiterGain() * 1000.0f);
        status->appliedGainTenths = static_cast<LONG>(gainDb * 10.0f + 0.5f);
        status->enabled = enabled ? 1 : 0;
        status->lastProcessTick = static_cast<LONG>(GetTickCount());
        InterlockedAdd64(&status->framesProcessed, frames);
    }
}

STDMETHODIMP_(UINT32) VolumeLiftApo::CalcInputFrames(UINT32 outputFrameCount) { return outputFrameCount; }

STDMETHODIMP_(UINT32) VolumeLiftApo::CalcOutputFrames(UINT32 inputFrameCount) { return inputFrameCount; }

}  // namespace vl
