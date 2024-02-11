//------------------------------------------------------------------------------
//! \file       ARAIPCCFEncoding.h
//!             Implementation of ARAIPCMessageEn-/Decoder backed by CF(Mutable)Dictionary
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

#ifndef ARAIPCCFEncoding_h
#define ARAIPCCFEncoding_h

#include "ARA_Library/IPC/ARAIPCMessage.h"

#if ARA_ENABLE_IPC


#include <CoreFoundation/CoreFoundation.h>


//! @addtogroup ARA_Library_IPC
//! @{


namespace ARA {
namespace IPC {


class CFMessageEncoder : public MessageEncoder
{
public:
    CFMessageEncoder ();
    ~CFMessageEncoder () override;

    void appendInt32 (MessageArgumentKey argKey, int32_t argValue) override;
    void appendInt64 (MessageArgumentKey argKey, int64_t argValue) override;
    void appendSize (MessageArgumentKey argKey, size_t argValue) override;
    void appendFloat (MessageArgumentKey argKey, float argValue) override;
    void appendDouble (MessageArgumentKey argKey, double argValue) override;
    void appendString (MessageArgumentKey argKey, const char * argValue) override;
    void appendBytes (MessageArgumentKey argKey, const uint8_t * argValue, size_t argSize, bool copy) override;
    MessageEncoder* appendSubMessage (MessageArgumentKey argKey) override;

    __attribute__((cf_returns_retained)) CFMutableDictionaryRef copyDictionary () const;
    __attribute__((cf_returns_retained)) CFDataRef createMessageEncoderData ()  const;

private:
    CFMutableDictionaryRef const _dictionary;
};


class CFMessageDecoder : public MessageDecoder
{
public:
    explicit CFMessageDecoder (CFDictionaryRef dictionary);
    ~CFMessageDecoder () override;

    bool readInt32 (MessageArgumentKey argKey, int32_t* argValue) const override;
    bool readInt64 (MessageArgumentKey argKey, int64_t* argValue) const override;
    bool readSize (MessageArgumentKey argKey, size_t* argValue) const override;
    bool readFloat (MessageArgumentKey argKey, float* argValue) const override;
    bool readDouble (MessageArgumentKey argKey, double* argValue) const override;
    bool readString (MessageArgumentKey argKey, const char ** argValue) const override;
    bool readBytesSize (MessageArgumentKey argKey, size_t* argSize) const override;
    void readBytes (MessageArgumentKey argKey, uint8_t* argValue) const override;
    MessageDecoder* readSubMessage (MessageArgumentKey argKey) const override;
    bool hasDataForKey (MessageArgumentKey argKey) const override;

    static CFMessageDecoder* createWithMessageData (CFDataRef messageData);

private:
    CFMessageDecoder (CFDictionaryRef dictionary, bool retain);

private:
    CFDictionaryRef const _dictionary;
};


}   // namespace IPC
}   // namespace ARA

//! @} ARA_Library_IPC

#endif // ARA_ENABLE_IPC

#endif // ARAIPCCFEncoding_h
