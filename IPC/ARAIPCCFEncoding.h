//------------------------------------------------------------------------------
//! \file       ARAIPCCFEncoding.h
//!             Implementation of ARAIPCMessageEn-/Decoder backed by CF(Mutable)Array
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

#ifndef ARAIPCCFEncoding_h
#define ARAIPCCFEncoding_h

#include "ARA_Library/IPC/ARAIPCMessage.h"

#if ARA_ENABLE_IPC


#include <CoreFoundation/CoreFoundation.h>
#include <memory>


//! @addtogroup ARA_Library_IPC
//! @{


namespace ARA {
namespace IPC {


class CFMessageEncoder : public MessageEncoder
{
public:
    ~CFMessageEncoder () override;

    void appendInt32 (int32_t argValue) override;
    void appendInt64 (int64_t argValue) override;
    void appendSize (size_t argValue) override;
    void appendFloat (float argValue) override;
    void appendDouble (double argValue) override;
    void appendBytes (const uint8_t* argValue, size_t argSize, bool copy) override;

    __attribute__((cf_returns_retained)) CFArrayRef copyArray () const;
    __attribute__((cf_returns_retained)) CFDataRef createMessageEncoderData ()  const;

    static std::unique_ptr<CFMessageEncoder> create () { return std::unique_ptr<CFMessageEncoder> { new CFMessageEncoder }; }

private:
    CFMessageEncoder ();

private:
    CFMutableArrayRef const _array;
};


class CFMessageDecoder : public MessageDecoder
{
public:
    CFMessageDecoder (CFArrayRef array, bool retain = true);
    ~CFMessageDecoder () override;

    bool readInt32 (int32_t& argValue) const override;
    bool readInt64 (int64_t& argValue) const override;
    bool readSize (size_t& argValue) const override;
    bool readFloat (float& argValue) const override;
    bool readDouble (double& argValue) const override;
    bool readBytes (const uint8_t*& argValue, size_t& argSize) const override;

    static std::unique_ptr<CFMessageDecoder> createWithMessageData (CFDataRef messageData);

private:
    CFArrayRef const _array;
    mutable CFIndex _index { 0 };
};


}   // namespace IPC
}   // namespace ARA

//! @} ARA_Library_IPC

#endif // ARA_ENABLE_IPC

#endif // ARAIPCCFEncoding_h
