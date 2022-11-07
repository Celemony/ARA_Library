//------------------------------------------------------------------------------
//! \file       ARAIPCLockingContext.h
//!             Implementation of locking IPC access to make it appear single-threaded
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

#ifndef ARAIPCLockingContext_h
#define ARAIPCLockingContext_h


#include "ARA_Library/IPC/ARAIPC.h"


//! @addtogroup ARA_Library_IPC
//! @{

#if ARA_ENABLE_IPC


#if defined(__cplusplus)
namespace ARA {
namespace IPC {
extern "C" {
#endif

//! Locking Context
//! The underlying IPC implementation must channel several threads through one or more single-threaded
//! IPC channel, requiring proper locking. At the same time, sending and receiving messages may be
//! distributed across different threads, and may be stacked, so proper locking depends on the specific
//! calls - for example, the host might call ARADocumentControllerInterface::notifyModelUpdates(),
//! causing the plug-in to respond with ARAModelUpdateControllerInterface.notifyAudioSourceContentChanged(),
//! which in turn triggers a host call to ARADocumentControllerInterface::isAudioSourceContentAvailable() etc.
//! At the same time, the host may make render calls through the Companion API and another thread in
//! the plug-in might be call ARAAudioAccessControllerInterface::readAudioSamples().
//! To handle this properly, the locking strategy is separated into this LockingContext class.
//! All communication that interacts in the non-IPC ARA world must use the same locking context,
//! i.e. all calls to/from a given document controller and all plug-in extensions bound to it.
//! \todo Are all Companion API calls independent of ARA, or might there be the need in certain
//!       situations to also lock around some Companion API calls?
//! \todo It is possible for a LockingContext instance to be used across several communication
//! channels if the IPC is distributed accordingly.
//! @{

//! opaque token representing an instance of a LockingContext
typedef struct ARAIPCLockingContextImplementation * ARAIPCLockingContextRef;

//! creation and destruction of LockingContext instances
//@{
ARAIPCLockingContextRef ARA_CALL ARAIPCCreateLockingContext (void);
void ARA_CALL ARAIPCDestroyLockingContext (ARAIPCLockingContextRef lockingContextRef);
//@}

//! locking around sending a message through the associated IPC channel(s)
//! The opaque token returned by the locking call must be passed on to the unlocking call.
//@{
typedef bool ARAIPCLockingContextMessageSendingToken;
ARAIPCLockingContextMessageSendingToken ARA_CALL ARAIPCLockContextBeforeSendingMessage (ARAIPCLockingContextRef lockingContextRef, bool stackable);
void ARA_CALL ARAIPCUnlockContextAfterSendingMessage (ARAIPCLockingContextRef lockingContextRef, ARAIPCLockingContextMessageSendingToken lockToken);
//@}

//! locking around handling a message received through the associated IPC channel(s)
//! The opaque token returned by the locking call must be passed on to the unlocking call.
//@{
typedef ARAIPCLockingContextRef ARAIPCLockingContextMessageHandlingToken;
ARAIPCLockingContextMessageHandlingToken ARA_CALL ARAIPCLockContextBeforeHandlingMessage (ARAIPCLockingContextRef lockingContextRef);
void ARA_CALL ARAIPCUnlockContextAfterHandlingMessage (ARAIPCLockingContextRef lockingContextRef, ARAIPCLockingContextMessageHandlingToken lockToken);
//@}

//! @}


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif


#endif // ARA_ENABLE_IPC

//! @} ARA_Library_IPC

#endif // ARAIPCLockingContext_h
