//------------------------------------------------------------------------------
//! \file       ARAIPCMessage.h
//!             implementation of message en-/decoding
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

#ifndef ARAIPCMessage_h
#define ARAIPCMessage_h

#include "ARA_Library/IPC/ARAIPC.h"

#if ARA_ENABLE_IPC


#include <memory>
#include <vector>

#if defined (__APPLE__)
    #include <CoreFoundation/CoreFoundation.h>
#endif


//! @addtogroup ARA_Library_IPC
//! @{

namespace ARA {
namespace IPC {


//! ID type for messages
typedef ARAInt32 MessageID;

//! the implementation reserves this range of IDs, any custom messages must be < kReservedMessageIDRangeStart or >= kReservedMessageIDRangeEnd
constexpr MessageID kReservedMessageIDRangeStart { 1 };
constexpr MessageID kReservedMessageIDRangeEnd { 8*16*16 };


//! Message Encoding
//! @{

//! base class for message encoders
class MessageEncoder
{
public:
    virtual ~MessageEncoder () = default;

    //! raw bytes
    //! As optimization, disable copying if the memory containing the bytes stays
    //! alive&unchanged until the message has been sent.
    //! Also used for appending numbers, strings etc.
    virtual void appendBytes (const uint8_t* bytes, size_t size, bool copy) = 0;

    //! extract data for sending through underlying IPC API
    //! must be called exactly once for every encoder
    virtual
#if defined (__APPLE__)
            __attribute__((cf_returns_retained)) CFDataRef
#else
            std::vector<uint8_t>
#endif
                                                           convertToEncodedData () = 0;
};


//! message encoder backed by fixed-size storage
class FixedSizeMessageEncoder : public MessageEncoder
{
public:
    FixedSizeMessageEncoder (size_t bufferSize);
    FixedSizeMessageEncoder (uint8_t* buffer, size_t bufferSize);

    ~FixedSizeMessageEncoder () override;

#if defined (__APPLE__)
    __attribute__((cf_returns_retained)) CFDataRef
#else
    std::vector<uint8_t>
#endif
                                                   convertToEncodedData () override;

    static std::unique_ptr<FixedSizeMessageEncoder> create ();

    void appendBytes (const uint8_t* bytes, size_t size, bool copy) override;

private:
    uint8_t* const _buffer;
    uint8_t* const _endOfBuffer;
    uint8_t* _writeLocation;
    const bool _ownsBuffer;
};


//! message encoder backed by std::vector
class VectorMessageEncoder : public MessageEncoder
{
public:
    VectorMessageEncoder (size_t capacity = 0);

#if defined (__APPLE__)
    __attribute__((cf_returns_retained)) CFDataRef
#else
    std::vector<uint8_t>
#endif
                                                   convertToEncodedData () override;

    static std::unique_ptr<VectorMessageEncoder> create ();

    void appendBytes (const uint8_t* bytes, size_t size, bool copy) override;

private:
    std::vector<uint8_t> _data;
};


#if defined (__APPLE__)
//! message encoder backed by CFData
class CFMessageEncoder : public MessageEncoder
{
public:
    CFMessageEncoder ();
    ~CFMessageEncoder () override;

    __attribute__((cf_returns_retained)) CFDataRef convertToEncodedData () override;

    static std::unique_ptr<CFMessageEncoder> create ();

    void appendBytes (const uint8_t* bytes, size_t size, bool copy) override;

private:
    CFMutableDataRef _data;
};
#endif

//! @}


//! Message Decoding
//! @{

//! message decoder backed by raw pointer and size
class MessageDecoder
{
public:
    MessageDecoder (const uint8_t* buffer, size_t bufferSize);

    virtual ~MessageDecoder () = default;

    //! raw bytes
    //! Will return a pointer to the message data or nullptr if the requested size exceeds
    //! the available archive data.
    //! Note that returned bytes pointer is only valid as long as the message that
    //! provided them is alive.
    //! Also used for reading numbers, strings etc.
    const uint8_t* readBytes (size_t size) const;

    static std::unique_ptr<MessageDecoder> createWithMessageData (const uint8_t* data, const size_t dataSize);

private:
    const uint8_t* const _buffer;
    const uint8_t* const _endOfBuffer;
    mutable const uint8_t* _readLocation;
};


#if defined (__APPLE__)
//! message decoder backed by CFData
class CFMessageDecoder : public MessageDecoder
{
public:
    explicit CFMessageDecoder (CFDataRef data, bool retain = true);
    ~CFMessageDecoder () override;

    static std::unique_ptr<CFMessageDecoder> createWithMessageData (CFDataRef messageData, bool isTemporary);

private:
    CFDataRef const _data;
};
#endif

//! @}

}   // namespace IPC
}   // namespace ARA

//! @} ARA_Library_IPC


#endif // ARA_ENABLE_IPC

#endif // ARAIPCMessage_h
