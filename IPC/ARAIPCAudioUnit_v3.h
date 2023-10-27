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


// using ARA IPC for Audio Units requires to compile for macOS 13 or higher
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


//! host side: initialize the message sender used for all factory-related messaging for all
//! Audio Units that have the same component as the provided audioUnit
//! will return nullptr if the Audio Unit does not implement [AUAudioUnit messageChannelFor:],
//! leaving the channel uninitialized
//! this sender can be used for the calls to ARAIPCAUProxyPlugInGetFactory(), ARAIPCProxyPlugInInitializeARA(),
//! ARAIPCProxyPlugInCreateDocumentControllerWithDocument() and ARAIPCProxyPlugInUninitializeARA()
ARAIPCMessageSender * _Nullable ARA_CALL ARAIPCAUProxyPlugInInitializeFactoryMessageSender(AUAudioUnit * _Nonnull audioUnit);

//! host side: get the ARA factory for the audio unit
//! will return NULL if the Audio Unit does not implement [AUAudioUnit messageChannelFor:]
const ARAFactory * _Nonnull ARA_CALL ARAIPCAUProxyPlugInGetFactory(ARAIPCMessageSender * _Nonnull messageSender);

//! host side: create the plug-in extension when performing the binding to the remote plug-in instance
//! also initializes the messageSender which remains valid until ARAIPCAUProxyPlugInCleanupBinding() is called
//! the document controller must be created through a factory obtained through ARAIPCAUProxyPlugInGetFactory()
//! will return NULL if the Audio Unit does not implement [AUAudioUnit messageChannelFor:],
//! leaving messageSender uninitialized in that case
const ARAPlugInExtensionInstance * _Nullable ARA_CALL ARAIPCAUProxyPlugInBindToDocumentController(AUAudioUnit * _Nonnull audioUnit,
                                                                                                  ARADocumentControllerRef _Nonnull documentControllerRef,
                                                                                                  ARAPlugInInstanceRoleFlags knownRoles,
                                                                                                  ARAPlugInInstanceRoleFlags assignedRoles,
                                                                                                  ARAIPCMessageSender * _Nullable * _Nonnull messageSender);

//! host side: trigger proper teardown of proxy plug-in extension when Companion API instance is destroyed
//! the messageSender must have been initialized by ARAIPCAUProxyPlugInBindToDocumentController() and will be uninitialized
void ARA_CALL ARAIPCAUProxyPlugInCleanupBinding(ARAIPCMessageSender * _Nonnull messageSender);

//! host side: uninitialize the sender set up in ARAIPCAUProxyPlugInInitializeFactoryMessageSender()
void ARA_CALL ARAIPCAUProxyPlugInUninitializeFactoryMessageSender(ARAIPCMessageSender * _Nonnull messageSender);


//! plug-in side: static configuration: add the ARA factories that the IPC shall handle
void ARA_CALL ARAIPCAUProxyHostAddFactory(const ARAFactory * _Nonnull factory);

//! callback that the proxy uses to execute the binding of an AUAudioUnit to the given document controller
typedef const ARAPlugInExtensionInstance * _Nonnull (^ARAIPCAUBindingHandler)(AUAudioUnit * _Nonnull audioUnit, ARADocumentControllerRef _Nonnull controllerRef,
                                                                              ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles);

//! callback that the proxy uses to ensure synchronous destruction of a an AUAudioUnit bound to ARA
//! this extra hook is necessary because the regular teardown via dealloc is delayed in macOS 13 on
//! the remote end, causing race conditions with ARA teardown methods send after plug-in destruction
typedef void (^ARAIPCAUDestructionHandler)(AUAudioUnit * _Nonnull audioUnit);

//! plug-in side: static configuration: after adding all factories, initialize the IPC (before allocating any instances)
void ARA_CALL ARAIPCAUProxyHostInitialize(NSObject<AUMessageChannel> * _Nonnull factoryMessageChannel,
                                          ARAIPCAUBindingHandler _Nonnull bindingHandler, ARAIPCAUDestructionHandler _Nonnull destructionHandler);

//! plug-in side: implementation for AUMessageChannel<NSObject> -callAudioUnit:
NSDictionary * _Nonnull ARA_CALL ARAIPCAUProxyHostCommandHandler (AUAudioUnit * _Nullable audioUnit, NSDictionary * _Nonnull message);

//! plug-in side: trigger proper teardown of proxy plug-in extension when Companion API instance is destroyed
void ARA_CALL ARAIPCAUProxyHostCleanupBinding(const ARAPlugInExtensionInstance * _Nonnull plugInExtensionInstance);

//! plug-in side: static cleanup upon shutdown
void ARA_CALL ARAIPCAUProxyHostUninitialize(void);


API_AVAILABLE_END


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif


#endif // ARA_AUDIOUNITV3_IPC_IS_AVAILABLE
