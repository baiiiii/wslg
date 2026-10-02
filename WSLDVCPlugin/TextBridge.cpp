// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "TextBridge.h"
#include "utils.h"

#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.RemoteDesktop.Input.h>

using namespace winrt::Windows::System::RemoteDesktop::Input;

namespace
{
    enum class BridgeChannelRole
    {
        ClientToServer,  // 插件→weston：系统文本输入服务发出的 PDU
        ServerToClient,  // weston→插件：喂给 ReportDataReceived 的 PDU
    };

    // 桥的全局状态：一条 RemoteTextConnection 连接 + 两条通道。
    // 生命周期与插件（msrdc 进程）相同，无需显式清理。
    RemoteTextConnection g_connection{ nullptr };
    winrt::com_ptr<IWTSVirtualChannel> g_spC2SChannel;
    winrt::com_ptr<IWTSVirtualChannel> g_spS2CChannel;

    // RemoteTextConnectionDataHandler：系统文本输入服务产生的每条
    // client→server PDU 在此回调，原样写往 weston 的 C2S 通道。
    // 返回 true 表示数据已消费。
    bool
    ForwardClientToServer(winrt::com_array<uint8_t> const& pdu)
    {
        DebugPrint(L"TextBridge: pduForwarder %u bytes\n", static_cast<ULONG>(pdu.size()));
        if (!g_spC2SChannel)
        {
            DebugPrint(L"TextBridge: C2S channel not ready, dropping\n");
            return true;
        }

        HRESULT hr = g_spC2SChannel->Write(
            static_cast<ULONG>(pdu.size()),
            const_cast<BYTE*>(pdu.data()),
            nullptr);
        if (FAILED(hr))
        {
            DebugPrint(L"TextBridge: C2S channel write failed hr=%x\n", hr);
        }
        return true;
    }

    // 通道数据回调：S2C 通道收到 weston 的 PDU 后喂给系统文本输入服务。
    struct TextBridgeChannelCallback :
        winrt::implements<TextBridgeChannelCallback, IWTSVirtualChannelCallback>
    {
        explicit TextBridgeChannelCallback(BridgeChannelRole role) :
            m_role(role)
        {
        }

        void AttachChannel(IWTSVirtualChannel* pChannel)
        {
            if (m_role == BridgeChannelRole::ClientToServer)
            {
                g_spC2SChannel.copy_from(pChannel);
            }
            else
            {
                g_spS2CChannel.copy_from(pChannel);
            }
            DebugPrint(L"TextBridge: channel connected (role=%d)\n", static_cast<int>(m_role));
        }

        // 只有 S2C 通道会收到 weston 的数据；C2S 通道的数据方向相反。
        STDMETHODIMP
        OnDataReceived(
            ULONG cbSize,
            _In_reads_(cbSize) BYTE* pBuffer)
        {
            if (m_role == BridgeChannelRole::ServerToClient && g_connection)
            {
                try
                {
                    auto pdu = winrt::com_array<uint8_t>(pBuffer, pBuffer + cbSize);
                    g_connection.ReportDataReceived(pdu);
                }
                catch (winrt::hresult_error const& e)
                {
                    DebugPrint(L"TextBridge: ReportDataReceived failed hr=%x %s\n",
                               e.code(), e.message().c_str());
                }
            }
            return S_OK;
        }

        STDMETHODIMP
        OnClose()
        {
            if (m_role == BridgeChannelRole::ClientToServer)
            {
                g_spC2SChannel = nullptr;
            }
            else
            {
                g_spS2CChannel = nullptr;
            }
            DebugPrint(L"TextBridge: channel closed (role=%d)\n", static_cast<int>(m_role));
            return S_OK;
        }

    private:
        BridgeChannelRole m_role;
    };

    // Listener：接受 weston（服务端）发起的通道创建请求。
    struct TextBridgeListenerCallback :
        winrt::implements<TextBridgeListenerCallback, IWTSListenerCallback>
    {
        explicit TextBridgeListenerCallback(BridgeChannelRole role) :
            m_role(role)
        {
        }

        STDMETHODIMP
        OnNewChannelConnection(
            __RPC__in_opt IWTSVirtualChannel* pChannel,
            __RPC__in_opt BSTR data,
            __RPC__out BOOL* pbAccept,
            __RPC__deref_out_opt IWTSVirtualChannelCallback** ppCallback)
        {
            UNREFERENCED_PARAMETER(data);

            auto callback = winrt::make<TextBridgeChannelCallback>(m_role);
            if (!callback)
            {
                *pbAccept = FALSE;
                return E_OUTOFMEMORY;
            }
            // 通道引用计数交给 msrdc；桥侧另存一份引用以便转发。
            callback->AttachChannel(pChannel);
            *ppCallback = callback.detach();
            *pbAccept = TRUE;
            return S_OK;
        }

    private:
        BridgeChannelRole m_role;
    };
}

HRESULT
TextBridge::Start(_In_ IWTSVirtualChannelManager* pChannelMgr)
{
    // WinRT 调用要求线程已初始化 apartment；若 msrdc 已按 STA 初始化
    // （RPC_E_CHANGED_MODE），保持原样即可，WinRT 对象默认 agile。
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (winrt::hresult_error const&)
    {
        // 线程已被初始化为其他 apartment 模型，直接沿用。
    }

    auto spC2SListener = winrt::make<TextBridgeListenerCallback>(BridgeChannelRole::ClientToServer);
    winrt::com_ptr<IWTSListener> spListener;
    HRESULT hr = pChannelMgr->CreateListener("WSL::TextBridge::ClientToServer", 0,
                                             spC2SListener.get(), spListener.put());
    if (FAILED(hr))
    {
        DebugPrint(L"TextBridge: CreateListener(ClientToServer) failed hr=%x\n", hr);
        return hr;
    }
    spListener = nullptr;

    auto spS2CListener = winrt::make<TextBridgeListenerCallback>(BridgeChannelRole::ServerToClient);
    hr = pChannelMgr->CreateListener("WSL::TextBridge::ServerToClient", 0,
                                     spS2CListener.get(), spListener.put());
    if (FAILED(hr))
    {
        DebugPrint(L"TextBridge: CreateListener(ServerToClient) failed hr=%x\n", hr);
        return hr;
    }
    spListener = nullptr;

    // 创建 RemoteTextConnection 并启用文本输入虚拟化。
    // connectionId 使用随机 GUID（规格未规定来源，官方客户端同样随机）。
    try
    {
        auto handler = RemoteTextConnectionDataHandler(&ForwardClientToServer);
        g_connection = RemoteTextConnection(
            winrt::Windows::Foundation::GuidHelper::CreateNewGuid(),
            handler);
        g_connection.IsEnabled(true);
        DebugPrint(L"TextBridge: RemoteTextConnection created, IsEnabled=%d\n",
                   g_connection.IsEnabled() ? 1 : 0);
    }
    catch (winrt::hresult_error const& e)
    {
        DebugPrint(L"TextBridge: create connection failed hr=%x %s\n",
                   e.code(), e.message().c_str());
        return e.code();
    }

    return S_OK;
}
