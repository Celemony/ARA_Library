//------------------------------------------------------------------------------
//! \file       ARAIPCCFEncoding.h
//!             Implementation of ARAIPCMessageEn-/Decoder backed by CF(Mutable)Dictionary
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

#ifndef ARAIPCCFEncoding_h
#define ARAIPCCFEncoding_h

#include "ARA_Library/IPC/ARAIPC.h"


#if ARA_ENABLE_IPC && defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>


namespace ARA {
namespace IPC {

class ARAIPCCFMessageEncoder : public ARAIPCMessageEncoder
{
public:
    ARAIPCCFMessageEncoder ();
    ~ARAIPCCFMessageEncoder () override;

    void appendInt32 (ARAIPCMessageKey argKey, int32_t argValue) override;
    void appendInt64 (ARAIPCMessageKey argKey, int64_t argValue) override;
    void appendSize (ARAIPCMessageKey argKey, size_t argValue) override;
    void appendFloat (ARAIPCMessageKey argKey, float argValue) override;
    void appendDouble (ARAIPCMessageKey argKey, double argValue) override;
    void appendString (ARAIPCMessageKey argKey, const char * argValue) override;
    void appendBytes (ARAIPCMessageKey argKey, const uint8_t * argValue, size_t argSize, bool copy) override;
    ARAIPCMessageEncoder* appendSubMessage (ARAIPCMessageKey argKey) override;

    __attribute__((cf_returns_retained)) CFMutableDictionaryRef copyDictionary () const;
    __attribute__((cf_returns_retained)) CFDataRef createMessageEncoderData ()  const;

private:
    CFMutableDictionaryRef const _dictionary;
};

class ARAIPCCFMessageDecoder : public ARAIPCMessageDecoder
{
public:
    explicit ARAIPCCFMessageDecoder (CFDictionaryRef dictionary);
    ~ARAIPCCFMessageDecoder () override;

    bool readInt32 (ARAIPCMessageKey argKey, int32_t* argValue) const override;
    bool readInt64 (ARAIPCMessageKey argKey, int64_t* argValue) const override;
    bool readSize (ARAIPCMessageKey argKey, size_t* argValue) const override;
    bool readFloat (ARAIPCMessageKey argKey, float* argValue) const override;
    bool readDouble (ARAIPCMessageKey argKey, double* argValue) const override;
    bool readString (ARAIPCMessageKey argKey, const char ** argValue) const override;
    bool readBytesSize (ARAIPCMessageKey argKey, size_t* argSize) const override;
    void readBytes (ARAIPCMessageKey argKey, uint8_t* argValue) const override;
    ARAIPCMessageDecoder* readSubMessage (ARAIPCMessageKey argKey) const override;
    bool hasDataForKey (ARAIPCMessageKey argKey) const override;

    static ARAIPCCFMessageDecoder* createWithMessageData (CFDataRef messageData);

private:
    ARAIPCCFMessageDecoder (CFDictionaryRef dictionary, bool retain);

private:
    CFDictionaryRef const _dictionary;
};

}   // namespace IPC
}   // namespace ARA

#endif // ARA_ENABLE_IPC && defined(__APPLE__)

#endif // ARAIPCCFEncoding_h
