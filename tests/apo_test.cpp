// Drives VolumeLiftApo.dll the way the audio engine does: class factory, format
// negotiation, LockForProcess and APOProcess. No registration or audio device needed.
// Usage: apo_test <path to VolumeLiftApo.dll>
#include <windows.h>
#include <mmreg.h>
#include <audiomediatype.h>
#include <audioenginebaseapo.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <vector>

#include "common/Config.h"

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) {
        ++g_failures;
    }
}

constexpr GUID kFloatSubtype = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kPcmSubtype = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// Minimal IAudioMediaType describing an uncompressed format.
class MediaType final : public IAudioMediaType {
public:
    MediaType(const GUID& subtype, UINT32 channels, UINT32 bytesPerSample, float rate) {
        format_.guidFormatType = subtype;
        format_.dwSamplesPerFrame = channels;
        format_.dwBytesPerSampleContainer = bytesPerSample;
        format_.dwValidBitsPerSample = bytesPerSample * 8;
        format_.fFramesPerSecond = rate;
        format_.dwChannelMask = channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
        wave_.wFormatTag = IsEqualGUID(subtype, kFloatSubtype) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
        wave_.nChannels = static_cast<WORD>(channels);
        wave_.nSamplesPerSec = static_cast<DWORD>(rate);
        wave_.wBitsPerSample = static_cast<WORD>(bytesPerSample * 8);
        wave_.nBlockAlign = static_cast<WORD>(channels * bytesPerSample);
        wave_.nAvgBytesPerSec = wave_.nSamplesPerSec * wave_.nBlockAlign;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioMediaType)) {
            *object = static_cast<IAudioMediaType*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override { return --refs_; }  // stack-owned in this test

    STDMETHODIMP IsCompressedFormat(BOOL* compressed) override {
        *compressed = FALSE;
        return S_OK;
    }
    STDMETHODIMP IsEqual(IAudioMediaType* /*other*/, DWORD* flags) override {
        *flags = 0;
        return S_FALSE;
    }
    STDMETHODIMP_(const WAVEFORMATEX*) GetAudioFormat() override { return &wave_; }
    STDMETHODIMP GetUncompressedAudioFormat(UNCOMPRESSEDAUDIOFORMAT* format) override {
        *format = format_;
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    UNCOMPRESSEDAUDIOFORMAT format_{};
    WAVEFORMATEX wave_{};
};

// Stand-in controlling unknown for the aggregation test; counts references routed to it.
class Outer final : public IUnknown {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        return inner ? inner->QueryInterface(riid, object) : E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    STDMETHODIMP_(ULONG) Release() override { return --refs; }
    IUnknown* inner = nullptr;
    ULONG refs = 1;
};

using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: apo_test <VolumeLiftApo.dll>\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE dll = LoadLibraryA(argv[1]);
    Check(dll != nullptr, "DLL loads");
    if (!dll) {
        return 1;
    }
    auto getClassObject = reinterpret_cast<GetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
    Check(getClassObject != nullptr, "DllGetClassObject exported");

    IClassFactory* factory = nullptr;
    Check(SUCCEEDED(getClassObject(vl::kApoClsid, IID_PPV_ARGS(&factory))), "class factory for the VolumeLift CLSID");
    IAudioProcessingObject* apo = nullptr;
    Check(factory && SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&apo))), "APO instance created");
    if (!apo) {
        return 1;
    }

    {
        Outer outer;
        IUnknown* inner = nullptr;
        IAudioProcessingObject* wrong = nullptr;
        Check(factory->CreateInstance(&outer, IID_PPV_ARGS(&wrong)) == CLASS_E_NOAGGREGATION,
              "aggregation refuses a non-IUnknown request");
        Check(SUCCEEDED(factory->CreateInstance(&outer, IID_PPV_ARGS(&inner))) && inner, "aggregated instance created");
        if (inner) {
            outer.inner = inner;
            IAudioProcessingObject* viaInner = nullptr;
            const ULONG before = outer.refs;
            Check(SUCCEEDED(inner->QueryInterface(IID_PPV_ARGS(&viaInner))) && outer.refs == before + 1,
                  "aggregated interfaces count references on the outer object");
            IAudioProcessingObjectRT* viaOuter = nullptr;
            Check(SUCCEEDED(viaInner->QueryInterface(IID_PPV_ARGS(&viaOuter))), "aggregated QI delegates to outer");
            if (viaOuter) {
                viaOuter->Release();
            }
            viaInner->Release();
            inner->Release();  // destroys the aggregated object
        }
    }

    IAudioSystemEffects* effects = nullptr;
    Check(SUCCEEDED(apo->QueryInterface(IID_PPV_ARGS(&effects))), "implements IAudioSystemEffects");
    if (effects) {
        effects->Release();
    }

    APO_REG_PROPERTIES* props = nullptr;
    Check(SUCCEEDED(apo->GetRegistrationProperties(&props)) && props && IsEqualGUID(props->clsid, vl::kApoClsid),
          "registration properties report the CLSID");
    CoTaskMemFree(props);
    Check(SUCCEEDED(apo->Initialize(0, nullptr)), "Initialize succeeds");

    MediaType floatStereo(kFloatSubtype, 2, 4, 48000.0f);
    MediaType pcm16(kPcmSubtype, 2, 2, 48000.0f);
    IAudioMediaType* supported = nullptr;
    Check(apo->IsInputFormatSupported(nullptr, &floatStereo, &supported) == S_OK && supported == &floatStereo,
          "float32 stereo input accepted");
    if (supported) {
        supported->Release();
    }
    supported = nullptr;
    Check(FAILED(apo->IsInputFormatSupported(nullptr, &pcm16, &supported)) && !supported, "16-bit PCM rejected");
    Check(apo->IsOutputFormatSupported(&floatStereo, &floatStereo, &supported) == S_OK, "matching output accepted");
    if (supported) {
        supported->Release();
    }

    constexpr UINT32 kFrames = 480;
    std::vector<float> buffer(kFrames * 2);
    APO_CONNECTION_DESCRIPTOR inDesc{};
    inDesc.Type = APO_CONNECTION_BUFFER_TYPE_EXTERNAL;
    inDesc.pBuffer = reinterpret_cast<UINT_PTR>(buffer.data());
    inDesc.u32MaxFrameCount = kFrames;
    inDesc.pFormat = &floatStereo;
    inDesc.u32Signature = APO_CONNECTION_DESCRIPTOR_SIGNATURE;
    APO_CONNECTION_DESCRIPTOR outDesc = inDesc;
    APO_CONNECTION_DESCRIPTOR* inputs[] = {&inDesc};
    APO_CONNECTION_DESCRIPTOR* outputs[] = {&outDesc};

    IAudioProcessingObjectConfiguration* config = nullptr;
    IAudioProcessingObjectRT* rt = nullptr;
    apo->QueryInterface(IID_PPV_ARGS(&config));
    apo->QueryInterface(IID_PPV_ARGS(&rt));
    Check(config && rt, "configuration and RT interfaces available");
    if (!config || !rt) {
        return 1;
    }
    Check(SUCCEEDED(config->LockForProcess(1, inputs, 1, outputs)), "LockForProcess succeeds");

    // Without a settings key the APO uses its defaults: enabled, +6 dB, limiter on.
    const float expectedGain = std::pow(10.0f, vl::kDefaultGainTenthsDb / 200.0f);
    float peak = 0.0f;
    for (int block = 0; block < 100; ++block) {  // 1 second
        for (UINT32 f = 0; f < kFrames; ++f) {
            const float value = 0.05f * std::sin(2.0f * 3.14159265f * 1000.0f * (block * kFrames + f) / 48000.0f);
            buffer[f * 2] = value;
            buffer[f * 2 + 1] = value;
        }
        APO_CONNECTION_PROPERTY in{};
        in.pBuffer = reinterpret_cast<UINT_PTR>(buffer.data());
        in.u32ValidFrameCount = kFrames;
        in.u32BufferFlags = BUFFER_VALID;
        in.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
        APO_CONNECTION_PROPERTY out = in;
        APO_CONNECTION_PROPERTY* inProps[] = {&in};
        APO_CONNECTION_PROPERTY* outProps[] = {&out};
        rt->APOProcess(1, inProps, 1, outProps);
        if (block == 99) {
            Check(out.u32ValidFrameCount == kFrames && out.u32BufferFlags == BUFFER_VALID,
                  "APOProcess reports a valid full block");
            for (float s : buffer) {
                peak = std::max(peak, std::fabs(s));
            }
        }
    }
    std::printf("output peak %.4f, expected %.4f\n", peak, 0.05f * expectedGain);
    Check(std::fabs(peak - 0.05f * expectedGain) < 0.003f, "APOProcess applies the default +6 dB boost");

    Check(SUCCEEDED(config->UnlockForProcess()), "UnlockForProcess succeeds");
    rt->Release();
    config->Release();
    apo->Release();
    factory->Release();
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
