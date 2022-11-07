//------------------------------------------------------------------------------
//! \file       ARAIPCLockingContext.cpp
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

#include "ARAIPCLockingContext.h"


#if ARA_ENABLE_IPC


#include "ARA_Library/Debug/ARADebug.h"

#include <mutex>
#include <thread>


namespace ARA {
namespace IPC {
extern "C" {


struct ARAIPCLockingContextImplementation
{
    std::mutex sendMutex {};
    std::mutex handleMutex {};
    bool sendIsStackable {};
};


thread_local ARAIPCLockingContextRef _currentHandlingLockingContextRef {};


ARAIPCLockingContextRef ARA_CALL ARAIPCCreateLockingContext (void)
{
    return new ARAIPCLockingContextImplementation;
}

void ARA_CALL ARAIPCDestroyLockingContext (ARAIPCLockingContextRef lockingContextRef)
{
    delete lockingContextRef;
}

ARAIPCLockingContextMessageSendingToken ARA_CALL ARAIPCLockContextBeforeSendingMessage (ARAIPCLockingContextRef lockingContextRef, bool stackable)
{
    const auto useLock { !lockingContextRef->sendIsStackable || (_currentHandlingLockingContextRef != lockingContextRef) };
    if (useLock)
    {
        lockingContextRef->sendMutex.lock ();
        lockingContextRef->sendIsStackable = stackable;
    }
    return useLock;
}

void ARA_CALL ARAIPCUnlockContextAfterSendingMessage (ARAIPCLockingContextRef lockingContextRef, ARAIPCLockingContextMessageSendingToken lockToken)
{
    if (lockToken)
    {
        lockingContextRef->sendIsStackable = false;
        lockingContextRef->sendMutex.unlock ();
    }
}

ARAIPCLockingContextMessageHandlingToken ARA_CALL ARAIPCLockContextBeforeHandlingMessage (ARAIPCLockingContextRef lockingContextRef)
{
    if (_currentHandlingLockingContextRef != lockingContextRef)
        lockingContextRef->handleMutex.lock ();
    const auto result { _currentHandlingLockingContextRef };
    _currentHandlingLockingContextRef = lockingContextRef;
    return result;
}

void ARA_CALL ARAIPCUnlockContextAfterHandlingMessage (ARAIPCLockingContextRef lockingContextRef, ARAIPCLockingContextMessageHandlingToken lockToken)
{
    ARA_INTERNAL_ASSERT (_currentHandlingLockingContextRef == lockingContextRef);
    if (lockingContextRef != lockToken)
        lockingContextRef->handleMutex.unlock ();
    _currentHandlingLockingContextRef = lockToken;
}


}   // extern "C"
}   // namespace IPC
}   // namespace ARA

#endif // ARA_ENABLE_IPC
