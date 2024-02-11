//------------------------------------------------------------------------------
//! \file       ARAIPCProxyHost.h
//!             implementation of host-side ARA IPC proxy host
//! \project    ARA SDK Library
//! \copyright  Copyright (c) 2021-2023, Celemony Software GmbH, All Rights Reserved.
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

#include "ARA_Library/IPC/ARAIPCMessageChannel.h"

#if ARA_ENABLE_IPC


//! @addtogroup ARA_Library_IPC
//! @{

#if defined(__cplusplus)
namespace ARA {
namespace IPC {
extern "C" {

//! plug-in side implementation of MessageHandler
class ProxyHostMessageHandler : public MessageHandler
{
public:
    ProxyHostMessageHandler ();

    DispatchTarget getDispatchTargetForIncomingTransaction (MessageID messageID) override;

    void handleReceivedMessage (MessageChannel* messageChannel,
                                const MessageID messageID, const MessageDecoder* const decoder,
                                MessageEncoder* const replyEncoder) override;

private:
    std::thread::id const _mainThreadID;
    DispatchTarget const _mainThreadDispatchTarget;
};
#endif


//! callback that the proxy uses to execute the binding of an opaque Companion API plug-in instance to the given document controller
typedef const ARAPlugInExtensionInstance * (*ARAIPCBindingHandler) (ARAIPCMessageChannelRef messageChannelRef, ARAIPCPlugInInstanceRef plugInInstanceRef,
                                                                    ARADocumentControllerRef controllerRef,
                                                                    ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles);

//! static configuration: add the ARA factories that the proxy host will wrap
void ARAIPCProxyHostAddFactory(const ARAFactory * factory);

//! static configuration: set the callback to execute the binding of Companion API plug-in instances to ARA document controllers
void ARAIPCProxyHostSetBindingHandler(ARAIPCBindingHandler handler);


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif

//! @} ARA_Library_IPC


#endif // ARA_ENABLE_IPC

#endif // ARAIPCProxyHost_h
