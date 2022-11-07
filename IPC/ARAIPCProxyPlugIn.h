//------------------------------------------------------------------------------
//! \file       ARAIPCProxyPlugIn.h
//!             implementation of host-side ARA IPC proxy plug-in
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

#ifndef ARAIPCProxyPlugIn_h
#define ARAIPCProxyPlugIn_h


#include "ARA_Library/IPC/ARAIPC.h"

#if ARA_ENABLE_IPC


#if defined(__cplusplus)
namespace ARA {
namespace IPC {
extern "C" {
#endif


//! counts the factories available through the message channel the sender is accessing
size_t ARAIPCProxyPlugInGetFactoriesCount(ARAIPCMessageSender hostCommandsSender);

//! get a static copy of the remote factory data, with all function calls removed
//! index must be smaller than the result of ARAIPCProxyPlugInGetFactoriesCount()
const ARAFactory * ARAIPCProxyPlugInGetFactoryAtIndex(ARAIPCMessageSender hostCommandsSender, size_t index);

//! proxy document controller creation call, to be used instead of ARAFactory.createDocumentControllerWithDocument()
const ARADocumentControllerInstance * ARAIPCProxyPlugInCreateDocumentControllerWithDocument(ARAIPCMessageSender hostCommandsSender,
                                                                                            const ARAPersistentID factoryID,
                                                                                            const ARADocumentControllerHostInstance * hostInstance,
                                                                                            const ARADocumentProperties * properties);

//! static handler of received messages
void ARAIPCProxyPlugInCallbacksDispatcher(const ARAIPCMessageID messageID, const ARAIPCMessageDecoder * decoder, ARAIPCMessageEncoder *  replyEncoder);

//! create the proxy plug-in extension when performing the binding to the remote plug-in instance
const ARAPlugInExtensionInstance * ARAIPCProxyPlugInBindToDocumentController(ARAIPCPlugInInstanceRef remoteRef,
                                                                            ARAIPCMessageSender sender, ARADocumentControllerRef documentControllerRef,
                                                                            ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles);

//! trigger proper teardown of proxy plug-in extension upon destroying a remote plug-in instance that has been bound to ARA
void ARAIPCProxyPlugInCleanupBinding(const ARAPlugInExtensionInstance * plugInExtension);


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif


#endif // ARA_ENABLE_IPC

#endif // ARAIPCProxyPlugIn_h
