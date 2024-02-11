//------------------------------------------------------------------------------
//! \file       ARAIPCAudioUnit_v3.h
//!             Implementation of ARA IPC message sending through AUMessageChannel
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


API_AVAILABLE_BEGIN(macos(13.0))


//! host side: initialize the message channel used for all factory-related messaging for all
//! Audio Units that have the same component as the provided audioUnit
//! will return nullptr if the Audio Unit does not implement [AUAudioUnit messageChannelFor:],
//! leaving the channel uninitialized
//! this channel can be used for the calls to ARAIPCAUProxyPlugInGetFactory(), ARAIPCProxyPlugInInitializeARA(),
//! ARAIPCProxyPlugInCreateDocumentControllerWithDocument() and ARAIPCProxyPlugInUninitializeARA()
ARAIPCMessageChannelRef _Nullable ARA_CALL ARAIPCAUProxyPlugInInitializeFactoryMessageChannel(AUAudioUnit * _Nonnull audioUnit);

//! host side: get the ARA factory for the audio unit
//! will return NULL if the Audio Unit does not implement [AUAudioUnit messageChannelFor:]
const ARAFactory * _Nonnull ARA_CALL ARAIPCAUProxyPlugInGetFactory(ARAIPCMessageChannelRef _Nonnull messageChannelRef);

//! host side: create the plug-in extension when performing the binding to the remote plug-in instance
//! also initializes the message channel, which remains valid until ARAIPCAUProxyPlugInCleanupBinding() is called
//! the document controller must be created through a factory obtained through ARAIPCAUProxyPlugInGetFactory()
//! will return NULL if the Audio Unit does not implement [AUAudioUnit messageChannelFor:],
//! leaving messageChannel uninitialized in that case
const ARAPlugInExtensionInstance * _Nullable ARA_CALL ARAIPCAUProxyPlugInBindToDocumentController(AUAudioUnit * _Nonnull audioUnit,
                                                                                                  ARADocumentControllerRef _Nonnull documentControllerRef,
                                                                                                  ARAPlugInInstanceRoleFlags knownRoles,
                                                                                                  ARAPlugInInstanceRoleFlags assignedRoles,
                                                                                                  ARAIPCMessageChannelRef _Nullable * _Nonnull messageChannelRef);

//! host side: trigger proper teardown of proxy plug-in extension when Companion API instance is destroyed
//! the message channel must have been initialized by ARAIPCAUProxyPlugInBindToDocumentController() and will be uninitialized
void ARA_CALL ARAIPCAUProxyPlugInCleanupBinding(ARAIPCMessageChannelRef _Nonnull messageChannelRef);

//! host side: uninitialize the channel set up in ARAIPCAUProxyPlugInInitializeFactoryMessageChannel()
void ARA_CALL ARAIPCAUProxyPlugInUninitializeFactoryMessageChannel(ARAIPCMessageChannelRef _Nonnull messageChannelRef);



//! plug-in side: static configuration: add the ARA factories that the IPC proxy shall handle
void ARA_CALL ARAIPCAUProxyHostAddFactory(const ARAFactory * _Nonnull factory);

//! plug-in side: static configuration: after adding all factories, initialize the IPC proxy (before allocating any instances)
void ARA_CALL ARAIPCAUProxyHostInitialize(NSObject<AUMessageChannel> * _Nonnull factoryMessageChannel);

//! plug-in side:implementation for AUMessageChannel<NSObject> -init...
ARAIPCMessageChannelRef _Nullable ARA_CALL ARAIPCAUProxyHostInitializeMessageChannel(AUAudioUnit * _Nonnull audioUnit,
                                                                                     NSObject<AUMessageChannel> * _Nonnull audioUnitChannel);

//! plug-in side: implementation for AUMessageChannel<NSObject> -callAudioUnit:
NSDictionary * _Nonnull ARA_CALL ARAIPCAUProxyHostCommandHandler(ARAIPCMessageChannelRef _Nonnull messageChannelRef, NSDictionary * _Nonnull message);

//! plug-in side:implementation for AUMessageChannel<NSObject> -dealloc
void ARA_CALL ARAIPCAUProxyHostUninitializeMessageChannel(ARAIPCMessageChannelRef _Nonnull messageChannelRef);

//! plug-in side: static cleanup upon shutdown
void ARA_CALL ARAIPCAUProxyHostUninitialize(void);


API_AVAILABLE_END


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif


//! @} ARA_Library_IPC

#endif // ARA_AUDIOUNITV3_IPC_IS_AVAILABLE
