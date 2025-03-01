//------------------------------------------------------------------------------
//! \file       ARAIPCProxyPlugIn.h
//!             implementation of host-side ARA IPC proxy plug-in
//! \project    ARA SDK Library
//! \copyright  Copyright (c) 2021-2025, Celemony Software GmbH, All Rights Reserved.
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

#if defined(__cplusplus)
#include "ARA_Library/IPC/ARAIPCConnection.h"
#include "ARA_Library/IPC/ARAIPCEncoding.h"
#else
#include "ARA_Library/IPC/ARAIPC.h"
#endif


#if ARA_ENABLE_IPC


//! @addtogroup ARA_Library_IPC
//! @{

#if defined(__cplusplus)
namespace ARA {
namespace IPC {
extern "C" {

//! host side implementation of MessageHandler
//! the host uses the C interface below, but this class will be subclassed by specialized implementations
class ProxyPlugIn : public MessageHandler, public RemoteCaller
{
public:
    using RemoteCaller::RemoteCaller;

    void handleReceivedMessage (const MessageID messageID, const MessageDecoder* const decoder,
                                MessageEncoder* const replyEncoder) override;
};
#endif


//! counts the factories available through the given message channel
size_t ARAIPCProxyPlugInGetFactoriesCount(ARAIPCConnectionRef connectionRef);

//! get a static copy of the remote factory data, with all function calls removed
//! index must be smaller than the result of ARAIPCProxyPlugInGetFactoriesCount()
const ARAFactory * ARAIPCProxyPlugInGetFactoryAtIndex(ARAIPCConnectionRef connectionRef, size_t index);

//! proxy initialization call, to be used instead of ARAFactory.initializeARAWithConfiguration()
// \todo we're currently not supporting propagating ARA assertions through IPC...
void ARAIPCProxyPlugInInitializeARA(ARAIPCConnectionRef connectionRef, const ARAPersistentID factoryID, ARAAPIGeneration desiredApiGeneration);

//! proxy document controller creation call, to be used instead of ARAFactory.createDocumentControllerWithDocument()
const ARADocumentControllerInstance * ARAIPCProxyPlugInCreateDocumentControllerWithDocument(ARAIPCConnectionRef connectionRef,
                                                                                            const ARAPersistentID factoryID,
                                                                                            const ARADocumentControllerHostInstance * hostInstance,
                                                                                            const ARADocumentProperties * properties);

//! create the proxy plug-in extension when performing the binding to the remote plug-in instance
const ARAPlugInExtensionInstance * ARAIPCProxyPlugInBindToDocumentController(ARAIPCPlugInInstanceRef remoteRef, ARADocumentControllerRef documentControllerRef,
                                                                             ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles);

//! trigger proper teardown of proxy plug-in extension upon destroying a remote plug-in instance that has been bound to ARA
void ARAIPCProxyPlugInCleanupBinding(const ARAPlugInExtensionInstance * plugInExtension);

//! proxy uninitialization call, to be used instead of ARAFactory.uninitializeARA()
void ARAIPCProxyPlugInUninitializeARA(ARAIPCConnectionRef connectionRef, const ARAPersistentID factoryID);


//! some companion APIs enforce processing the ARA IPC calls on threads other than the main thread -
//! this call returns true when the current thread is the actual main thread or some IPC thread
//! that acts on behalf of the main thread (similar to Swift MainActor)
ARA_DRAFT ARABool ARAIPCProxyPlugInCurrentThreadActsAsMainThread ();


//! draft for synchronizing host and plug-in main thread
//! the lock must be taken each time the main thread wakes up, in order to prevent the plug-in main
//! thread from interfering with the main thread operation in the host
//! try-lock can be used for operations which can be simply aborted if the plug-in is currently busy,
//! e.g. when a repeating timer fires
ARA_DRAFT void ARAIPCProxyPlugInLockDistributedMainThread(void);
ARA_DRAFT ARABool ARAIPCProxyPlugInTryLockDistributedMainThread(void);
ARA_DRAFT void ARAIPCProxyPlugInUnlockDistributedMainThread(void);


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif

//! @} ARA_Library_IPC


#endif // ARA_ENABLE_IPC

#endif // ARAIPCProxyPlugIn_h
