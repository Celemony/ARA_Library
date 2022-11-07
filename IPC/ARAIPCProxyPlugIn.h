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


typedef struct ARAIPCProxyPlugInFactory ARAIPCProxyPlugInFactory;


//! static initialization
size_t ARAIPCProxyPlugInInitializeFactories(ARAIPCMessageSender hostCommandsSender);

//! access proxies to the factories provided by the remote plug-in
ARAIPCProxyPlugInFactory * ARAIPCProxyPlugInGetFactoryAtIndex(size_t index);

//! get copy of the remote factory data, with all function calls removed
const ARAFactory * ARAIPCProxyPlugInGetFactoryData(ARAIPCProxyPlugInFactory * proxyFactory);

//! proxy document controller creation call, to be used instead of ARAFactory.createDocumentControllerWithDocument()
const ARADocumentControllerInstance * ARAIPCProxyPlugInCreateDocumentControllerWithDocument(ARAIPCProxyPlugInFactory * proxyFactory,
                                                                                            const ARADocumentControllerHostInstance * hostInstance,
                                                                                            const ARADocumentProperties * properties);

//! static handler of received messages
void ARAIPCProxyPlugInCallbacksDispatcher(const ARAIPCMessageID messageID, const ARAIPCMessageDecoder * decoder, ARAIPCMessageEncoder *  replyEncoder);

//! \todo to perform the binding to the remote plug-in instance, the host needs access to this translation...
ARADocumentControllerRef ARAIPCProxyPlugInTranslateDocumentControllerRef(ARADocumentControllerRef documentControllerRef);

//! create the plug-in extension when performing the binding to the remote plug-in instance
const ARAPlugInExtensionInstance * ARAIPCProxyPlugInCreatePlugInExtension(size_t remoteExtensionRef, ARAIPCMessageSender sender, ARADocumentControllerRef documentControllerRef,
                                                                          ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles);
//! destroy the plug-in extension when destroying the remote plug-in instance
void ARAIPCProxyPlugInDestroyPlugInExtension(const ARAPlugInExtensionInstance * plugInExtension);


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif


#endif // ARA_ENABLE_IPC

#endif // ARAIPCProxyPlugIn_h
