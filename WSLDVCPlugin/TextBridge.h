// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "pch.h"
#include <tsvirtualchannels.h>

// WSLg 文本输入桥：实现 MS-RDPETXT 规格所述"扩展 DLL"的角色——
// 在 RDP 客户端（msrdc）进程内，把 Windows 系统文本输入服务
// （TSF3 IME 宿主）与 WSLg RDP 服务端（weston rdptext.c 实现的
// MS-RDPETXT 协议）对接，使宿主机输入法可为远程编辑控件工作。
namespace TextBridge
{
    // 在 WSLDVCPlugin::Initialize 中调用：
    //   1. 注册两个自定义 DVC 通道监听（weston 服务端会主动打开它们）：
    //        WSL::TextBridge::ClientToServer  承载 client→server 方向的 PDU
    //        WSL::TextBridge::ServerToClient  承载 server→client 方向的 PDU
    //      通道名未沿用规格点名的 TextInput_*DVC，因为这两个名字已被
    //      msrdc 内置 remotetextplugin 注册监听（其 RemoteTextConnection
    //      处于 IsEnabled=false 的禁用状态）；通道内消息逐字节仍是
    //      MS-RDPETXT 协议。
    //   2. 创建 RemoteTextConnection（公开 API）并置 IsEnabled=true，
    //      以系统文本输入服务作为协议客户端驱动整个集成。
    HRESULT Start(_In_ IWTSVirtualChannelManager* pChannelMgr);
}
