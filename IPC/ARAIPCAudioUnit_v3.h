//------------------------------------------------------------------------------
//! \file       ARAIPCAudioUnit_v3.h
//!             Implementation of ARA IPC message sending through AUMessageChannel
//! \project    ARA SDK Library
//! \copyright  Copyright (c) 2021-2024, Celemony Software GmbH, All Rights Reserved.
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

//! host side: an opaque encapsulation to represent a Audio Unit ARA proxy plug-in component,
//! to be used for all Audio Unit instances from the same Audio Unit Component
//! (pools the Audio Unit message channels for use across all AU instances)
typedef ARA_IPC_REF(ARAIPCConnectionRef);

//! host side: initialize proxy plug-in component and its internal the message channels
//! will return nullptr if the Audio Unit does not implement [AUAudioUnit messageChannelFor:]
//! for the required ARA message channels
//! the provided audioUnit is only used to establish the channels, it can be closed again
//! after the call if not needed otherwise
//! must be balanced with ARAIPCAUProxyPlugInUninitialize()
ARAIPCConnectionRef _Nullable ARA_CALL ARAIPCAUProxyPlugInInitialize(AUAudioUnit * _Nonnull audioUnit);

//! host side: query the message channel that can be used for the calls to ARAIPCAUProxyPlugInGetFactory(),
//! ARAIPCProxyPlugInInitializeARA(), ARAIPCProxyPlugInCreateDocumentControllerWithDocument()
//! and ARAIPCProxyPlugInUninitializeARA()
ARAIPCMessageChannelRef _Nonnull ARA_CALL ARAIPCAUProxyPlugInGetMainMessageChannel(ARAIPCConnectionRef _Nonnull proxyRef);

//! host side: Audio Unit specialization of ARAIPCProxyPlugInBindToDocumentController()
//! must be balanced with ARAIPCProxyPlugInCleanupBinding() when the given audioUnit is destroyed
const ARAPlugInExtensionInstance * _Nonnull ARA_CALL ARAIPCAUProxyPlugInBindToDocumentController(AUAudioUnit * _Nonnull audioUnit,
                                                                                                 ARADocumentControllerRef _Nonnull documentControllerRef,
                                                                                                 ARAPlugInInstanceRoleFlags knownRoles,
                                                                                                 ARAPlugInInstanceRoleFlags assignedRoles);

//! host side: uninitialize the proxy component set up in ARAIPCAUProxyPlugInInitialize()
void ARA_CALL ARAIPCAUProxyPlugInUninitialize(ARAIPCConnectionRef _Nonnull proxyRef);



//! plug-in side: static configuration: after adding all factories via ARAIPCProxyHostAddFactory(),
//! initialize the IPC proxy (before allocating any instances)
void ARA_CALL ARAIPCAUProxyHostInitialize(NSObject<AUMessageChannel> * _Nonnull factoryMessageChannel);

//! plug-in side:implementation for AUMessageChannel<NSObject> -init...
ARAIPCMessageChannelRef _Nullable ARA_CALL ARAIPCAUProxyHostInitializeMessageChannel(NSObject<AUMessageChannel> * _Nonnull audioUnitChannel);

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
