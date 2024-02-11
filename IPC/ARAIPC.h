//------------------------------------------------------------------------------
//! \file       ARAIPC.h
//!             C-compatible declarations of externally visible IPC types
//!             Typically, this file is not included directly - either ARAIPCProxyHost.h
//!             ARAIPCProxyPlugIn.h will be used instead.
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

#ifndef ARAIPC_h
#define ARAIPC_h

#include "ARA_API/ARAInterface.h"


//! @addtogroup ARA_Library_IPC
//! @{

//! switch to bypass all IPC code
#if !defined (ARA_ENABLE_IPC)
    #if defined (__APPLE__) || defined (_WIN32)
        #define ARA_ENABLE_IPC 1
    #else
        #define ARA_ENABLE_IPC 0
    #endif
#endif

#if ARA_ENABLE_IPC


#if defined(__cplusplus)
namespace ARA {
namespace IPC {
extern "C" {

#if !defined (ARA_MAP_IPC_REF)
    #define ARA_MAP_IPC_REF(IPCClassType, FirstIPCRefType, ...) \
        static inline ARA::ToRefConversionHelper<IPCClassType, FirstIPCRefType, ##__VA_ARGS__> toIPCRef (const IPCClassType* ptr) noexcept { return ARA::ToRefConversionHelper<IPCClassType, FirstIPCRefType, ##__VA_ARGS__> { ptr }; } \
        template <typename DesiredIPCClassType = IPCClassType, typename IPCRefType, typename std::enable_if<std::is_constructible<ARA::FromRefConversionHelper<IPCClassType, FirstIPCRefType, ##__VA_ARGS__>, IPCRefType>::value, bool>::type = true> \
        static inline DesiredIPCClassType* fromIPCRef (IPCRefType ref) noexcept { IPCClassType* object { ARA::FromRefConversionHelper<IPCClassType, FirstIPCRefType, ##__VA_ARGS__> (ref) }; return static_cast<DesiredIPCClassType*> (object); }
#endif

    //! helper define to properly insert ARA::IPC namespace into C compatible headers
    #define ARA_IPC_NAMESPACE ARA::IPC::
#else
    #define ARA_IPC_NAMESPACE
#endif


//! IPC reference markup type identifier. \br\br
//! Examples:                           \br
//! ::ARAIPCPlugInInstanceRef           \br
//! ::ARAIPCMessageChannelRef           \br
#define ARA_IPC_REF(IPCRefType) struct IPCRefType##MarkupType * IPCRefType


//! C-compatible wrapper of MessageChannel
typedef ARA_IPC_REF(ARAIPCMessageChannelRef);


//! to keep the IPC decoupled from the Companion API in use, the IPC code uses
//! an opaque encapsulation to represent a Companion API plug-in instance
typedef ARA_IPC_REF(ARAIPCPlugInInstanceRef);


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif

#endif // ARA_ENABLE_IPC

//! @} ARA_Library_IPC

#endif // ARAIPC_h
