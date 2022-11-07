//------------------------------------------------------------------------------
//! \file       ARAIPCProxyHost.h
//!             implementation of host-side ARA IPC proxy host
//! \project    ARA SDK Examples
//! \copyright  Copyright (c) 2021-2022, Celemony Software GmbH, All Rights Reserved.
//! \license    Licensed under the Apache License, Version 2.0 (the "License");
//!             you may not use this file except in compliance with the License.
//!             You may obtain a copy of the License at
//!
//!               http://www.apache.org/licenses/LICENSE-2.0
//!
//!             Unless required by applicable law or agreed to in writing, software
//!             distributed under the License is distributed on an "AS IS" BASIS,
//!             WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//!             See the License for the specific language governing permissions and
//!             limitations under the License.
//------------------------------------------------------------------------------

#ifndef ARAIPCProxyHost_h
#define ARAIPCProxyHost_h

#include "ARA_Library/IPC/ARAIPC.h"


#if ARA_ENABLE_IPC

#if defined(__cplusplus)
namespace ARA {
namespace IPC {
extern "C" {
#endif


//! static configuration: add the ARA factories that the proxy host will wrap
void ARAIPCProxyHostAddFactory(const ARAFactory * factory);

//! static configuration: set sender that the proxy host will use to perform callbacks received from the plug-in
void ARAIPCProxyHostSetPlugInCallbacksSender(ARAIPCMessageSender plugInCallbacksSender);

//! static configuration: set the callback to execute the binding of Companion API plug-in instances to ARA document controllers
void ARAIPCProxyHostSetBindingHandler(ARAIPCBindingHandler handler);

//! static dispatcher: the host command handler that controls the proxy host
void ARAIPCProxyHostCommandHandler(const ARAIPCMessageID messageID, const ARAIPCMessageDecoder * decoder, ARAIPCMessageEncoder * replyEncoder);

//! trigger proper teardown of proxy plug-in extension when destroying Companion API plug-in instances that have been bound to ARA
void ARAIPCProxyHostCleanupBinding(const ARAPlugInExtensionInstance * plugInExtensionInstance);


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif

#endif // ARA_ENABLE_IPC

#endif // ARAIPCProxyHost_h
