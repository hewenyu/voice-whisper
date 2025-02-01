#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <initguid.h>

// 定义GUID
DEFINE_GUID(IID_IPolicyConfig,
    0xf8679f50, 0x850a, 0x41cf, 0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8);

DEFINE_GUID(CLSID_CPolicyConfigClient,
    0x870af99c, 0x171d, 0x4f9e, 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9);

// 定义接口
#ifdef __cplusplus
interface IPolicyConfig : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(
        PCWSTR,
        WAVEFORMATEX **
    ) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(
        PCWSTR,
        INT,
        WAVEFORMATEX **
    ) = 0;

    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(
        PCWSTR
    ) = 0;

    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(
        PCWSTR,
        WAVEFORMATEX *,
        WAVEFORMATEX *
    ) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(
        PCWSTR,
        INT,
        PINT64,
        PINT64
    ) = 0;

    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(
        PCWSTR,
        PINT64
    ) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetShareMode(
        PCWSTR,
        struct DeviceShareMode *
    ) = 0;

    virtual HRESULT STDMETHODCALLTYPE SetShareMode(
        PCWSTR,
        struct DeviceShareMode *
    ) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(
        PCWSTR,
        const PROPERTYKEY &,
        PROPVARIANT *
    ) = 0;

    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(
        PCWSTR,
        const PROPERTYKEY &,
        PROPVARIANT *
    ) = 0;

    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(
        PCWSTR wszDeviceId,
        ERole eRole
    ) = 0;

    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(
        PCWSTR,
        INT
    ) = 0;

    virtual HRESULT STDMETHODCALLTYPE RegisterAudioEndpoint(
        PCWSTR deviceId,
        PCWSTR deviceName,
        PCWSTR deviceDesc,
        EDataFlow flow,
        DWORD state,
        GUID* moduleId
    ) = 0;

    virtual HRESULT STDMETHODCALLTYPE UnregisterAudioEndpoint(
        PCWSTR deviceId
    ) = 0;
};
#endif 