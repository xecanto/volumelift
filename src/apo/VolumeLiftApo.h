// System-effect audio processing object (APO) that applies VolumeLift's boost.
// Implemented directly against the audio engine interfaces from the Windows SDK.
#pragma once

#include <windows.h>
#include <mmreg.h>
#include <audiomediatype.h>
#include <audioenginebaseapo.h>

#include <atomic>

#include "apo/SettingsWatcher.h"
#include "apo/StatusSection.h"
#include "dsp/Booster.h"

namespace vl {

class VolumeLiftApo final : public IAudioProcessingObject,
                            public IAudioProcessingObjectRT,
                            public IAudioProcessingObjectConfiguration,
                            public IAudioSystemEffects {
public:
    // `outer` is the controlling unknown when the audio engine aggregates the APO, else null.
    explicit VolumeLiftApo(IUnknown* outer);
    ~VolumeLiftApo();

    // The non-delegating IUnknown, owning the object's lifetime. Holds one reference on creation.
    IUnknown* Inner() { return &inner_; }

    // IUnknown, delegating to the controlling unknown.
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IAudioProcessingObject
    STDMETHODIMP Reset() override;
    STDMETHODIMP GetLatency(HNSTIME* time) override;
    STDMETHODIMP GetRegistrationProperties(APO_REG_PROPERTIES** properties) override;
    STDMETHODIMP Initialize(UINT32 cbDataSize, BYTE* pbyData) override;
    STDMETHODIMP IsInputFormatSupported(IAudioMediaType* opposite, IAudioMediaType* requested,
                                        IAudioMediaType** supported) override;
    STDMETHODIMP IsOutputFormatSupported(IAudioMediaType* opposite, IAudioMediaType* requested,
                                         IAudioMediaType** supported) override;
    STDMETHODIMP GetInputChannelCount(UINT32* count) override;

    // IAudioProcessingObjectRT
    STDMETHODIMP_(void) APOProcess(UINT32 inputCount, APO_CONNECTION_PROPERTY** inputs, UINT32 outputCount,
                                   APO_CONNECTION_PROPERTY** outputs) override;
    STDMETHODIMP_(UINT32) CalcInputFrames(UINT32 outputFrameCount) override;
    STDMETHODIMP_(UINT32) CalcOutputFrames(UINT32 inputFrameCount) override;

    // IAudioProcessingObjectConfiguration
    STDMETHODIMP LockForProcess(UINT32 inputCount, APO_CONNECTION_DESCRIPTOR** inputs, UINT32 outputCount,
                                APO_CONNECTION_DESCRIPTOR** outputs) override;
    STDMETHODIMP UnlockForProcess() override;

    // Fills the APO registration record shared by GetRegistrationProperties and DllRegisterServer.
    static void FillRegistration(APO_REG_PROPERTIES& props);

private:
    class InnerUnknown final : public IUnknown {
    public:
        explicit InnerUnknown(VolumeLiftApo* owner) : owner_(owner) {}
        STDMETHODIMP QueryInterface(REFIID riid, void** object) override;
        STDMETHODIMP_(ULONG) AddRef() override;
        STDMETHODIMP_(ULONG) Release() override;

    private:
        VolumeLiftApo* owner_;
        std::atomic<ULONG> refs_{1};
    };

    InnerUnknown inner_{this};
    IUnknown* outer_;
    bool locked_ = false;
    UINT32 channels_ = 0;
    Booster booster_;
    SettingsWatcher settings_;
    StatusSection status_;
};

// Number of live objects and server locks, for DllCanUnloadNow.
extern std::atomic<long> g_serverRefs;

}  // namespace vl
