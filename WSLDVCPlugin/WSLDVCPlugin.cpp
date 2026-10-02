// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "WSLDVCPlugin.h"
#include "WSLDVCListenerCallback.h"
#include "TextBridge.h"
#include "utils.h"

//
// Using Windows Runtime C++ Template Library(WRL) to implement COM objects.
// See:
//    https://docs.microsoft.com/en-us/cpp/cppcx/wrl/how-to-instantiate-wrl-components-directly?view=vs-2019
//    https://docs.microsoft.com/en-us/cpp/cppcx/wrl/how-to-create-a-classic-com-component-using-wrl?view=vs-2019
//

class WSLDVCPlugin :
    public RuntimeClass<
    RuntimeClassFlags<ClassicCom>,
    IWTSPlugin>
{
public:

    HRESULT
        RuntimeClassInitialize()
    {
        return S_OK;
    }

    //
    // IWTSPlugin interface
    //
    STDMETHODIMP
        Initialize(
            __RPC__in_opt IWTSVirtualChannelManager* pChannelMgr
        )
    {
        HRESULT hr = S_OK;
        ComPtr<IWTSListenerCallback> spListenerCallback;
        ComPtr<IWTSListener> spListener;

        hr = WSLDVCListenerCallback_CreateInstance(&spListenerCallback);
        if (SUCCEEDED(hr))
        {
            hr = pChannelMgr->CreateListener(DVC_NAME, 0, spListenerCallback.Get(), &spListener);
        }

        // WSLg 文本输入桥：注册文本桥通道并创建 RemoteTextConnection
        // （MS-RDPETXT 规格的客户端扩展组件角色），使宿主机输入法可为
        // 远程编辑控件工作。桥失败不影响应用列表通道。
        if (SUCCEEDED(hr))
        {
            HRESULT hrBridge = TextBridge::Start(pChannelMgr);
            if (FAILED(hrBridge))
            {
                DebugPrint(L"TextBridge::Start failed hr=%x (text input virtualization disabled)\n", hrBridge);
            }
        }

        return hr;
    }

    STDMETHODIMP
        Connected()
    {
        return S_OK;
    }

    STDMETHODIMP
        Disconnected(
            DWORD dwDisconnectCode
        )
    {
        UNREFERENCED_PARAMETER(dwDisconnectCode);
        return S_OK;
    }

    STDMETHODIMP
        Terminated()
    {
        return S_OK;
    }

protected:

    virtual
        ~WSLDVCPlugin()
    {
    }

private:

    const char* DVC_NAME = "Microsoft::Windows::RDS::RemoteApplicationList";
};

HRESULT
WSLDVCPlugin_CreateInstance(
    IWTSPlugin** ppPlugin
)
{
    return MakeAndInitialize<WSLDVCPlugin>(ppPlugin);
}
