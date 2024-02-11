//------------------------------------------------------------------------------
//! \file       ARAIPCProxyPlugIn.h
//!             implementation of host-side ARA IPC proxy plug-in
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

#ifndef ARAIPCProxyPlugIn_h
#define ARAIPCProxyPlugIn_h

#include "ARA_Library/IPC/ARAIPCMessageChannel.h"

#if ARA_ENABLE_IPC


//! @addtogroup ARA_Library_IPC
//! @{

#if defined(__cplusplus)
namespace ARA {
namespace IPC {
extern "C" {

//! host side implementation of ARAIPCMessageHandler
class ARAIPCProxyPlugInMessageHandler : public ARAIPCMessageHandler
{
public:
    using ARAIPCMessageHandler::ARAIPCMessageHandler;

    DispatchTarget getDispatchTargetForIncomingTransaction (ARAIPCMessageID messageID) override;

    void handleReceivedMessage (ARAIPCMessageChannel* messageChannel,
                                const ARAIPCMessageID messageID, const ARAIPCMessageDecoder* const decoder,
                                ARAIPCMessageEncoder* const replyEncoder) override;
};
#endif


//! counts the factories available through the given message channel
size_t ARAIPCProxyPlugInGetFactoriesCount(ARAIPCMessageChannelRef messageChannelRef);

//! get a static copy of the remote factory data, with all function calls removed
//! index must be smaller than the result of ARAIPCProxyPlugInGetFactoriesCount()
const ARAFactory * ARAIPCProxyPlugInGetFactoryAtIndex(ARAIPCMessageChannelRef messageChannelRef, size_t index);

//! proxy initialization call, to be used instead of ARAFactory.initializeARAWithConfiguration()
// \todo we're currently not supporting propagating ARA assertions through IPC...
void ARAIPCProxyPlugInInitializeARA(ARAIPCMessageChannelRef messageChannelRef, const ARAPersistentID factoryID, ARAAPIGeneration desiredApiGeneration);

//! proxy document controller creation call, to be used instead of ARAFactory.createDocumentControllerWithDocument()
const ARADocumentControllerInstance * ARAIPCProxyPlugInCreateDocumentControllerWithDocument(ARAIPCMessageChannelRef messageChannelRef,
                                                                                            const ARAPersistentID factoryID,
                                                                                            const ARADocumentControllerHostInstance * hostInstance,
                                                                                            const ARADocumentProperties * properties);

//! create the proxy plug-in extension when performing the binding to the remote plug-in instance
const ARAPlugInExtensionInstance * ARAIPCProxyPlugInBindToDocumentController(ARAIPCPlugInInstanceRef remoteRef,
                                                                             ARAIPCMessageChannelRef messageChannelRef, ARADocumentControllerRef documentControllerRef,
                                                                             ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles);

//! trigger proper teardown of proxy plug-in extension upon destroying a remote plug-in instance that has been bound to ARA
void ARAIPCProxyPlugInCleanupBinding(const ARAPlugInExtensionInstance * plugInExtension);

//! proxy uninitialization call, to be used instead of ARAFactory.uninitializeARA()
void ARAIPCProxyPlugInUninitializeARA(ARAIPCMessageChannelRef messageChannelRef, const ARAPersistentID factoryID);


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif

//! @} ARA_Library_IPC


#endif // ARA_ENABLE_IPC

#endif // ARAIPCProxyPlugIn_h
