//------------------------------------------------------------------------------
//! \file       ARAIPCAudioUnit_v3.h
//!             Implementation of ARA IPC message sending through AUMessageChannel
//! \project    ARA SDK Library
//! \copyright  Copyright (c) 2021-2026, Celemony Software GmbH, All Rights Reserved.
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

#import "ARA_API/ARAAudioUnit_v3.h"


//! @addtogroup ARA_Library_IPC
//! @{


//! using ARA IPC for Audio Units requires to compile for macOS 13 or higher
#if defined(__MAC_13_0)
    #define ARA_AUDIOUNITV3_IPC_IS_AVAILABLE 1
#else
    #define ARA_AUDIOUNITV3_IPC_IS_AVAILABLE 0
#endif

#if ARA_AUDIOUNITV3_IPC_IS_AVAILABLE


#import "ARA_Library/IPC/ARAIPC.h"


#if defined(__cplusplus)
namespace ARA {
namespace IPC {
extern "C" {
#endif


API_AVAILABLE_BEGIN(macos(13.0), ios(16.0))

//! Optionally, the host can request that the proxy synchronizes the two main threads by blocking
//! either the host or the plug-in run loop at any given point. This requires the host to run its
//! own loop regularly, in order not to block the plug-in loop for an extended period of time.
//! To enable the sync, the host should call this method from its main() implementation,
//! before calling into NSApplicationMain(). It needs to provide the run loop mode that the should
//! be used when blocking either side while the other side is running.
void ARAIPCAUProxyPlugInPrepareRunLoopSync (CFRunLoopMode _Nonnull runLoopSyncPrivateMode);

//! host side: initialize proxy plug-in component and its internal the message channels
//! will return nullptr if the Audio Unit does not implement [AUAudioUnit messageChannelFor:]
//! for the required ARA message channels
//! the provided audioUnit is only used to establish the channels, it can be closed again
//! after the call if not needed otherwise
//! must be balanced with ARAIPCAUProxyPlugInUninitialize()
//! the optional delegate will be called periodically when the IPC needs to block the main thread,
//! with the opaque user data pointer being passed back into the call
ARAIPCProxyPlugInRef _Nullable ARA_CALL ARAIPCAUProxyPlugInInitialize(AUAudioUnit * _Nonnull audioUnit);

//! allows the host to let the plug-in perform ARA IPC on the main thread when otherwise
//! blocking it for an extended period of time
void ARA_CALL ARAIPCAUProxyPlugInPerformPendingMainThreadTasks(ARAIPCProxyPlugInRef _Nonnull proxyPlugInRef);

//! Some Audio Unit calls can trigger ARA callbacks, most prominently allocateRenderResourcesAndReturnError:
//! and deallocateRenderResources. However, the macOS performs these calls in a blocking fashion,
//! preventing any callbacks from coming through on the calling thread. This wrapper works around
//! this problem by moving the Audio Unit call to a different thread, keeping the calling thread
//! alive for callbacks. It synchronously waits for completion of the Audio Unit call before continuing.
void ARA_CALL ARAIPCAUProxyPlugInCallAudioUnitAllowingCallbacks(ARAIPCProxyPlugInRef _Nonnull proxyPlugInRef,
                                                                void (^ _Nonnull audioUnitCall)(void));

//! host side: Audio Unit specialization of ARAIPCProxyPlugInBindToDocumentController()
//! must be balanced with ARAIPCProxyPlugInCleanupBinding() when the given audioUnit is destroyed
const ARAPlugInExtensionInstance * _Nonnull ARA_CALL ARAIPCAUProxyPlugInBindToDocumentController(AUAudioUnit * _Nonnull audioUnit,
                                                                                                 ARADocumentControllerRef _Nonnull documentControllerRef,
                                                                                                 ARAPlugInInstanceRoleFlags knownRoles,
                                                                                                 ARAPlugInInstanceRoleFlags assignedRoles);

//! host side: uninitialize the proxy component set up in ARAIPCAUProxyPlugInInitialize()
void ARA_CALL ARAIPCAUProxyPlugInUninitialize(ARAIPCProxyPlugInRef _Nonnull proxyPlugInRef);



//! plug-in side: Once the host enables the run loop sync via ARAIPCAUProxyPlugInPrepareRunLoopSync(),
//! this will block any main thread access while the host is running its main thread.
//! However, some calls in the AUv3 API still need to be forwarded to the main thread synchronously,
//! so this function must be used in the AUv3 layer instead of direct dispatch_sync() calls.
//! Note that this function is not thread safe - it assumes that this is used for calls that the
//! host only will make sequentially (i.e. from main thread or from synchronized init thread).
void ARA_CALL ARAIPCAUProxyHostDispatchSyncToMainThread(void (^_Nonnull block)(void));

//! plug-in side: implementation for AUAudioUnit messageChannelFor:
id<AUMessageChannel> _Nullable ARA_CALL ARAIPCAUProxyHostMessageChannelFor(NSString * _Nonnull channelName);


API_AVAILABLE_END


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif


//! @} ARA_Library_IPC

#endif // ARA_AUDIOUNITV3_IPC_IS_AVAILABLE
