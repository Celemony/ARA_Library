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

    //! helper define to properly insert ARA::IPC namespace into C compatible headers
    #define ARA_IPC_NAMESPACE ARA::IPC::
#else
    #define ARA_IPC_NAMESPACE
#endif


//! Companion API: opaque encapsulation
//! @{
//! to keep the IPC decoupled from the Companion API in use, the IPC code uses an opaque token to represent a plug-in instance
typedef size_t ARAIPCPlugInInstanceRef;
//! @}


#if defined(__cplusplus)
class ARAIPCMessageChannel;
#else
typedef struct ARAIPCMessageChannel ARAIPCMessageChannel;
#endif


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif

#endif // ARA_ENABLE_IPC

//! @} ARA_Library_IPC

#endif // ARAIPC_h
