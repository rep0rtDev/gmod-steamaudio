// src/platform/PlatformAudioOutput_Windows.cpp
//
// WASAPI shared-mode, event-driven render endpoint. The device thread waits
// on the buffer event, pulls stereo float frames from the owner and converts
// them into the endpoint's mix format (float32 or PCM 16/24/32, any channel
// count; channels beyond the first two receive silence except for a mono
// downmix on mono endpoints). Default-endpoint changes and device
// invalidation are reported through NeedsRestart().
#include "platform/PlatformAudioOutput.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// initguid.h makes this TU instantiate the WASAPI/KS GUIDs and PROPERTYKEYs
// we compare against, so no ksuser/uuid import library variant is required.
#include <initguid.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include "util/Logging.h"

namespace sa {

namespace {

// WAVE_FORMAT_EXTENSIBLE sub-format GUIDs (ksmedia.h declares them extern on
// some toolchains, which would need ksuser.lib).
constexpr GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kSubtypeIeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T** Put()
    {
        Reset();
        return &m_ptr;
    }
    void** PutVoid() { return reinterpret_cast<void**>(Put()); }
    T* Get() const { return m_ptr; }
    T* operator->() const { return m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }
    void Reset()
    {
        if (m_ptr) {
            m_ptr->Release();
            m_ptr = nullptr;
        }
    }

private:
    T* m_ptr = nullptr;
};

struct CoTaskMemDeleter {
    void operator()(void* p) const { CoTaskMemFree(p); }
};

std::string WideToUtf8(const wchar_t* wide)
{
    if (!wide)
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 1)
        return {};
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), len);
    return out;
}

enum class SampleLayout { Float32, Pcm16, Pcm24, Pcm32, Unsupported };

SampleLayout ClassifyFormat(const WAVEFORMATEX* fmt)
{
    WORD tag = fmt->wFormatTag;
    WORD bits = fmt->wBitsPerSample;
    WORD validBits = bits;
    if (tag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        validBits = ext->Samples.wValidBitsPerSample ? ext->Samples.wValidBitsPerSample : bits;
        if (IsEqualGUID(ext->SubFormat, kSubtypeIeeeFloat))
            tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (IsEqualGUID(ext->SubFormat, kSubtypePcm))
            tag = WAVE_FORMAT_PCM;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && bits == 32)
        return SampleLayout::Float32;
    if (tag == WAVE_FORMAT_PCM) {
        if (bits == 16)
            return SampleLayout::Pcm16;
        if (bits == 24)
            return SampleLayout::Pcm24;
        if (bits == 32)
            return validBits == 24 ? SampleLayout::Pcm32 : SampleLayout::Pcm32;
    }
    return SampleLayout::Unsupported;
}

class WasapiAudioOutput;

// Receives default-device change notifications from the MMDevice enumerator.
class NotificationClient final : public IMMNotificationClient {
public:
    explicit NotificationClient(std::atomic<bool>& flag) : m_flag(flag) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++m_refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = static_cast<ULONG>(--m_refs);
        if (r == 0)
            delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
    {
        if (flow == eRender && (role == eConsole || role == eMultimedia))
            m_flag.store(true, std::memory_order_release);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    std::atomic<long> m_refs{1};
    std::atomic<bool>& m_flag;
};

class WasapiAudioOutput final : public IPlatformAudioOutput {
public:
    ~WasapiAudioOutput() override { Stop(); }

    bool Start(const OutputRequest& request, IOutputSource& source, OutputFormat& actual,
               std::string& error) override
    {
        Stop();
        m_request = request;
        m_source = &source;
        m_needsRestart.store(false, std::memory_order_release);
        m_startError.clear();
        m_startResult.store(0, std::memory_order_release);
        m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_wakeEvent) {
            error = "CreateEvent failed";
            m_source = nullptr;
            return false;
        }
        m_running.store(true, std::memory_order_release);
        m_thread = std::thread([this] { Run(); });

        // Wait for the device thread to finish initialisation (bounded).
        for (int i = 0; i < 500 && m_startResult.load(std::memory_order_acquire) == 0; ++i)
            Sleep(10);
        const int result = m_startResult.load(std::memory_order_acquire);
        if (result != 1) {
            Stop();
            error = result == 0 ? "WASAPI initialisation timed out" : m_startError;
            return false;
        }
        actual = m_format;
        return true;
    }

    void Stop() override
    {
        m_running.store(false, std::memory_order_release);
        if (m_wakeEvent)
            SetEvent(m_wakeEvent);
        if (m_thread.joinable())
            m_thread.join();
        if (m_wakeEvent) {
            CloseHandle(m_wakeEvent);
            m_wakeEvent = nullptr;
        }
        m_source = nullptr;
    }

    bool IsRunning() const override { return m_running.load(std::memory_order_acquire) && m_started; }
    bool NeedsRestart() const override { return m_needsRestart.load(std::memory_order_acquire); }
    const OutputFormat& Format() const override { return m_format; }
    uint64_t FramesDelivered() const override { return m_delivered.load(std::memory_order_relaxed); }
    uint64_t Underruns() const override { return m_underruns.load(std::memory_order_relaxed); }
    const char* BackendName() const override { return "wasapi"; }

private:
    bool Initialize(ComPtr<IMMDeviceEnumerator>& enumerator, ComPtr<IMMDevice>& device,
                    ComPtr<IAudioClient>& client, ComPtr<IAudioRenderClient>& render, std::string& error)
    {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), enumerator.PutVoid());
        if (FAILED(hr)) {
            error = "CoCreateInstance(MMDeviceEnumerator) failed: 0x" + HexString(hr);
            return false;
        }
        if (!m_request.deviceId.empty()) {
            hr = enumerator->GetDevice(Utf8ToWide(m_request.deviceId).c_str(), device.Put());
            if (FAILED(hr))
                SA_LOGW("[wasapi] device '%s' not found (0x%08lx), using default", m_request.deviceId.c_str(), hr);
        }
        if (!device) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.Put());
            if (FAILED(hr)) {
                error = "no default render endpoint: 0x" + HexString(hr);
                return false;
            }
        }
        // Friendly name for logs.
        {
            ComPtr<IPropertyStore> props;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.Put()))) {
                PROPVARIANT name;
                PropVariantInit(&name);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
                    m_format.deviceName = WideToUtf8(name.pwszVal);
                PropVariantClear(&name);
            }
        }
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.PutVoid());
        if (FAILED(hr)) {
            error = "IMMDevice::Activate(IAudioClient) failed: 0x" + HexString(hr);
            return false;
        }

        WAVEFORMATEX* mixRaw = nullptr;
        hr = client->GetMixFormat(&mixRaw);
        if (FAILED(hr) || !mixRaw) {
            error = "GetMixFormat failed: 0x" + HexString(hr);
            return false;
        }
        std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter> mix(mixRaw);
        m_layout = ClassifyFormat(mix.get());
        if (m_layout == SampleLayout::Unsupported) {
            error = "unsupported endpoint mix format (tag " + std::to_string(mix->wFormatTag) + ", " +
                    std::to_string(mix->wBitsPerSample) + " bit)";
            return false;
        }
        m_deviceChannels = mix->nChannels;
        m_bytesPerSample = mix->wBitsPerSample / 8;

        REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
        client->GetDevicePeriod(&defaultPeriod, &minPeriod);
        // Request a buffer of ~3 periods, at least the caller's frame size.
        const double requestedSec = std::max(static_cast<double>(m_request.frameSize) / mix->nSamplesPerSec,
                                             static_cast<double>(defaultPeriod) * 1e-7);
        const REFERENCE_TIME bufferDuration = static_cast<REFERENCE_TIME>(requestedSec * 3.0 * 1e7);

        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferDuration, 0, mix.get(), nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 frames = 0;
            client->GetBufferSize(&frames);
            const REFERENCE_TIME aligned =
                static_cast<REFERENCE_TIME>(1e7 * static_cast<double>(frames) / mix->nSamplesPerSec + 0.5);
            client.Reset();
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.PutVoid());
            if (SUCCEEDED(hr))
                hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, aligned, 0, mix.get(), nullptr);
        }
        if (FAILED(hr)) {
            error = "IAudioClient::Initialize failed: 0x" + HexString(hr);
            return false;
        }
        hr = client->GetBufferSize(&m_bufferFrames);
        if (FAILED(hr)) {
            error = "GetBufferSize failed: 0x" + HexString(hr);
            return false;
        }
        hr = client->SetEventHandle(m_wakeEvent);
        if (FAILED(hr)) {
            error = "SetEventHandle failed: 0x" + HexString(hr);
            return false;
        }
        hr = client->GetService(__uuidof(IAudioRenderClient), render.PutVoid());
        if (FAILED(hr)) {
            error = "GetService(IAudioRenderClient) failed: 0x" + HexString(hr);
            return false;
        }

        m_format.bufferFrames = static_cast<int32_t>(m_bufferFrames);
        m_format.sampleRate = static_cast<int32_t>(mix->nSamplesPerSec);
        m_format.channels = 2;
        m_format.periodFrames = static_cast<int32_t>(std::max<UINT32>(
            64, static_cast<UINT32>(static_cast<double>(defaultPeriod) * 1e-7 * mix->nSamplesPerSec + 0.5)));

        // Pre-fill with silence so the first period does not glitch.
        BYTE* data = nullptr;
        if (SUCCEEDED(render->GetBuffer(m_bufferFrames, &data)))
            render->ReleaseBuffer(m_bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);

        hr = client->Start();
        if (FAILED(hr)) {
            error = "IAudioClient::Start failed: 0x" + HexString(hr);
            return false;
        }

        m_notify = new NotificationClient(m_needsRestart);
        if (FAILED(enumerator->RegisterEndpointNotificationCallback(m_notify))) {
            m_notify->Release();
            m_notify = nullptr;
        }

        SA_LOGI("[wasapi] '%s': %d Hz, %u device channels, %s, buffer %u frames, period %d", m_format.deviceName.c_str(),
                m_format.sampleRate, m_deviceChannels, LayoutName(m_layout), m_bufferFrames, m_format.periodFrames);
        return true;
    }

    static const char* LayoutName(SampleLayout l)
    {
        switch (l) {
        case SampleLayout::Float32: return "float32";
        case SampleLayout::Pcm16: return "pcm16";
        case SampleLayout::Pcm24: return "pcm24";
        case SampleLayout::Pcm32: return "pcm32";
        default: return "unsupported";
        }
    }

    static std::string HexString(HRESULT hr)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(hr));
        return buf;
    }

    void Convert(const float* stereo, BYTE* out, UINT32 frames) const
    {
        const UINT32 ch = m_deviceChannels;
        const size_t stride = static_cast<size_t>(ch) * m_bytesPerSample;
        for (UINT32 f = 0; f < frames; ++f) {
            float l = stereo[f * 2];
            float r = stereo[f * 2 + 1];
            l = std::max(-1.f, std::min(1.f, l));
            r = std::max(-1.f, std::min(1.f, r));
            BYTE* frame = out + f * stride;
            for (UINT32 c = 0; c < ch; ++c) {
                float v = 0.f;
                if (ch == 1)
                    v = 0.5f * (l + r);
                else if (c == 0)
                    v = l;
                else if (c == 1)
                    v = r;
                BYTE* dst = frame + c * m_bytesPerSample;
                switch (m_layout) {
                case SampleLayout::Float32: {
                    std::memcpy(dst, &v, sizeof(float));
                    break;
                }
                case SampleLayout::Pcm16: {
                    const int16_t s = static_cast<int16_t>(std::lrint(v * 32767.f));
                    std::memcpy(dst, &s, sizeof(s));
                    break;
                }
                case SampleLayout::Pcm24: {
                    const int32_t s = static_cast<int32_t>(std::lrint(v * 8388607.f));
                    dst[0] = static_cast<BYTE>(s & 0xFF);
                    dst[1] = static_cast<BYTE>((s >> 8) & 0xFF);
                    dst[2] = static_cast<BYTE>((s >> 16) & 0xFF);
                    break;
                }
                case SampleLayout::Pcm32: {
                    const int32_t s = static_cast<int32_t>(std::lrint(static_cast<double>(v) * 2147483647.0));
                    std::memcpy(dst, &s, sizeof(s));
                    break;
                }
                default: break;
                }
            }
        }
    }

    void Run()
    {
        SetCurrentThreadName("sa-wasapi");
        const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        struct Apartment {
            bool initialized;
            ~Apartment() { if (initialized) CoUninitialize(); }
        } apartment{SUCCEEDED(coInit)};

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        std::string error;
        if (!Initialize(enumerator, device, client, render, error)) {
            m_startError = error;
            SA_LOGE("[wasapi] %s", error.c_str());
            Cleanup(enumerator, client);
            m_startResult.store(2, std::memory_order_release);
            return;
        }
        m_started = true;
        m_startResult.store(1, std::memory_order_release);
        ElevateCurrentThreadToAudioPriority("sa-wasapi");

        std::vector<float> stereo(static_cast<size_t>(m_bufferFrames) * 2);
        while (m_running.load(std::memory_order_acquire)) {
            const DWORD wait = WaitForSingleObject(m_wakeEvent, 2000);
            if (!m_running.load(std::memory_order_acquire))
                break;
            if (wait == WAIT_TIMEOUT) {
                // The device stopped delivering events: treat as lost.
                SA_LOGW("[wasapi] buffer event timeout; requesting restart");
                m_needsRestart.store(true, std::memory_order_release);
                break;
            }
            UINT32 padding = 0;
            HRESULT hr = client->GetCurrentPadding(&padding);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
                SA_LOGW("[wasapi] device invalidated (0x%08lx)", hr);
                m_needsRestart.store(true, std::memory_order_release);
                break;
            }
            if (FAILED(hr))
                continue;
            const UINT32 available = m_bufferFrames > padding ? m_bufferFrames - padding : 0;
            if (available == 0)
                continue;
            if (padding == 0)
                m_underruns.fetch_add(1, std::memory_order_relaxed);
            BYTE* data = nullptr;
            hr = render->GetBuffer(available, &data);
            if (FAILED(hr)) {
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                    m_needsRestart.store(true, std::memory_order_release);
                    break;
                }
                continue;
            }
            m_source->PullStereo(stereo.data(), available);
            Convert(stereo.data(), data, available);
            render->ReleaseBuffer(available, 0);
            m_delivered.fetch_add(available, std::memory_order_relaxed);
        }

        client->Stop();
        m_started = false;
        Cleanup(enumerator, client);
    }

    void Cleanup(ComPtr<IMMDeviceEnumerator>& enumerator, ComPtr<IAudioClient>& client)
    {
        if (m_notify) {
            if (enumerator)
                enumerator->UnregisterEndpointNotificationCallback(m_notify);
            m_notify->Release();
            m_notify = nullptr;
        }
        (void)client;
    }

    OutputRequest m_request;
    OutputFormat m_format;
    IOutputSource* m_source = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_needsRestart{false};
    std::atomic<int> m_startResult{0}; // 0 pending, 1 ok, 2 failed
    std::string m_startError;
    std::atomic<uint64_t> m_delivered{0};
    std::atomic<uint64_t> m_underruns{0};
    std::atomic<bool> m_started{false};
    HANDLE m_wakeEvent = nullptr;
    NotificationClient* m_notify = nullptr;
    UINT32 m_bufferFrames = 0;
    UINT32 m_deviceChannels = 2;
    UINT32 m_bytesPerSample = 4;
    SampleLayout m_layout = SampleLayout::Float32;
};

} // namespace

std::unique_ptr<IPlatformAudioOutput> CreateWasapiAudioOutput()
{
    return std::make_unique<WasapiAudioOutput>();
}

std::unique_ptr<IPlatformAudioOutput> CreateNativeAudioOutput()
{
    return CreateWasapiAudioOutput();
}

} // namespace sa

#endif // _WIN32
