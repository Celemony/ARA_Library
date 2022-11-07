//------------------------------------------------------------------------------
//! \file       ARAIPCCFEncoding.h
//!             Implementation of ARAIPCMessageEn-/Decoder backed by CF(Mutable)Dictionary
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

#ifndef ARAIPCCFEncoding_h
#define ARAIPCCFEncoding_h

#include "ARA_Library/IPC/ARAIPC.h"


#if ARA_ENABLE_IPC && defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>


#if defined(__cplusplus)
namespace ARA {
namespace IPC {
extern "C" {
#endif


ARAIPCMessageEncoder ARAIPCCFCreateMessageEncoder(void);
__attribute__((cf_returns_retained)) CFDataRef ARAIPCCFCreateMessageEncoderData(ARAIPCMessageEncoderRef messageEncoderRef);

ARAIPCMessageDecoder ARAIPCCFCreateMessageDecoder(CFDataRef messageData);


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif


#endif // ARA_ENABLE_IPC && defined(__APPLE__)

#endif // ARAIPCCFEncoding_h
