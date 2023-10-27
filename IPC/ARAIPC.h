//------------------------------------------------------------------------------
//! \file       ARAIPC.h
//!             Abstractions shared by both the ARA IPC proxy host and plug-in
//!             Typically, this file is not included directly - either ARAIPCProxyHost.h
//!             ARAIPCProxyPlugIn.h will be used instead.
//! \project    ARA SDK Library
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

#ifndef ARAIPC_h
#define ARAIPC_h


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


#include <functional>


namespace ARA {
namespace IPC {


//! ID type for messages, IDs must be >= kMessageIDRangeStart and < kMessageIDRangeEnd
using MessageID = int32_t;
constexpr MessageID kMessageIDRangeStart { 1 };
constexpr MessageID kMessageIDRangeEnd { 8*16*16 - 1 };

//! key type for message dictionaries - negative keys are reserved for the implementation
using MessageKey = int32_t;


class MessageEncoder
{
public:
    virtual ~MessageEncoder () = default;

    //! number types
    //! The size variant will also be used for the pointer-sized ARA (host) refs.
    //@{
    virtual void appendInt32 (const MessageKey argKey, const int32_t argValue) = 0;
    virtual void appendInt64 (const MessageKey argKey, const int64_t argValue) = 0;
    virtual void appendSize (const MessageKey argKey, const size_t argValue) = 0;
    virtual void appendFloat (const MessageKey argKey, const float argValue) = 0;
    virtual void appendDouble (const MessageKey argKey, const double argValue) = 0;
    //@}

    //! UTF8-encoded C strings
    virtual void appendString (const MessageKey argKey, const char* const argValue) = 0;

    //! raw bytes
    //! As optimization, disable copying if the memory containing the bytes stays
    //! alive&unchanged until the message has been sent.
    virtual void appendBytes (const MessageKey argKey, const uint8_t* argValue, const size_t argSize, const bool copy = true) = 0;

    //! sub-messages to encode compound types
    //! The caller is responsible for deleting the encoder after use.
    virtual MessageEncoder* appendSubMessage (const MessageKey argKey) = 0;
};

class MessageDecoder
{
public:
    virtual ~MessageDecoder () = default;

    //! only for debugging/validation: test if the message contains any key/value pairs
    virtual bool isEmpty () const = 0;

    //! number types
    //! The size variant will also be used for the pointer-sized ARA (host) refs.
    //! Will return false and set argValue to 0 if key not found.
    //@{
    virtual bool readInt32 (const MessageKey argKey, int32_t& argValue) const = 0;
    virtual bool readInt64 (const MessageKey argKey, int64_t& argValue) const = 0;
    virtual bool readSize (const MessageKey argKey, size_t& argValue) const = 0;
    virtual bool readFloat (const MessageKey argKey, float& argValue) const = 0;
    virtual bool readDouble (const MessageKey argKey, double& argValue) const = 0;
    //@}

    //! UTF8-encoded C strings
    //! Will return false and set argValue to nullptr if key not found.
    virtual bool readString (const MessageKey argKey, const char*& argValue) const = 0;

    //! raw bytes
    //! first query size, then provide a buffer large enough to copy the bytes to.
    //! readBytesSize () will return false and set argSize to 0 if key not found.
    //@{
    virtual bool readBytesSize (const MessageKey argKey, size_t& argSize) const = 0;
    virtual void readBytes (const MessageKey argKey, uint8_t* const argValue) const = 0;
    //@}

    //! sub-messages to decode compound types
    //! returns nullptr if key not found or if the value for the key is not representing a sub-message
    //! The caller is responsible for deleting the encoder after use.
    virtual MessageDecoder* readSubMessage (const MessageKey argKey) const = 0;
};

//! receive function: receives a message readable through the decoder, optionally creating a reply
//! Not using the replyEncoder will return a valid empty message to the sender (useful for void calls).
//! Depending on the underlying implementation, replyEncoder may be nullptr if no reply has been
//! requested by the sender, but providing a dummy encoder in this case is valid too.
//! The sender thread will be blocked until the (possibly empty) reply has been received.
//! A receive function can be called from any thread, but not concurrently.
using ReceiveFunction = void (const MessageID messageID, const MessageDecoder& decoder, MessageEncoder* const replyEncoder);

//! gateway for sending messages
class Sender
{
public:
    virtual ~Sender () = default;

    using ReplyHandler = std::function<void (const MessageDecoder& decoder)>;

    //! generate an encoder to encode a new message
    //! An encoder can be reused if the same message is sent several times,
    //! but it must not be modified after sending.
    //! The caller is responsible for deleting the encoder after use.
    virtual MessageEncoder* createEncoder () = 0;

    //! send function: send message create using the encoder, blocking until a reply has been received.
    //! If an empty reply ("void") is expected, the replyHandler should be nullptr.
    //! A send function can be called from any thread, but not concurrently.
    virtual void sendMessage (MessageID messageID, const MessageEncoder& encoder, ReplyHandler* const replyHandler) = 0;

    //! Test if the receiver runs on a different architecture with different endianess.
    virtual bool receiverEndianessMatches () const { return true; }
};


}   // namespace IPC
}   // namespace ARA

#endif // ARA_ENABLE_IPC

//! @} ARA_Library_IPC

#endif // ARAIPC_h
