//------------------------------------------------------------------------------
//! \file       ARAIPC.h
//!             Abstractions shared by both the ARA IPC proxy host and plug-in
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
#endif


//! ID type for messages, IDs must be >= kARAIPCMessageIDRangeStart and < kARAIPCMessageIDRangeEnd
typedef ARAInt32 ARAIPCMessageID;
#if defined(__cplusplus)
    constexpr ARAIPCMessageID kARAIPCMessageIDRangeStart { 1 };
    constexpr ARAIPCMessageID kARAIPCMessageIDRangeEnd { 8*16*16 - 1 };
#else
    #define kARAIPCMessageIDRangeStart  ((ARAIPCMessageID) 1)
    #define kARAIPCMessageIDRangeEnd ((ARAIPCMessageID) 8*16*16 - 1)
#endif


//! key type for message dictionaries - negative keys are reserved for the implementation
typedef ARAInt32 ARAIPCMessageKey;


//! Message Encoder
//! @{
#if defined(__cplusplus)
class ARAIPCMessageEncoder
{
public:
    virtual ~ARAIPCMessageEncoder() = default;

    //! number types
    //! The "size" variant will also be used for the pointer-sized ARA (host) refs.
    //@{
    virtual void appendInt32 (ARAIPCMessageKey argKey, int32_t argValue) = 0;
    virtual void appendInt64 (ARAIPCMessageKey argKey, int64_t argValue) = 0;
    virtual void appendSize (ARAIPCMessageKey argKey, size_t argValue) = 0;
    virtual void appendFloat (ARAIPCMessageKey argKey, float argValue) = 0;
    virtual void appendDouble (ARAIPCMessageKey argKey, double argValue) = 0;
    //@}

    //! UTF8-encoded C strings
    virtual void appendString (ARAIPCMessageKey argKey, const char* argValue) = 0;

    //! raw bytes
    //! As optimization, disable copying if the memory containing the bytes stays
    //! alive&unchanged until the message has been sent.
    virtual void appendBytes (ARAIPCMessageKey argKey, const uint8_t* argValue, size_t argSize, bool copy) = 0;

    //! sub-messages to encode compound types
    //! The caller is responsible for deleting the returned sub-encoder after use.
    virtual ARAIPCMessageEncoder* appendSubMessage (ARAIPCMessageKey argKey) = 0;
};
#else
typedef struct ARAIPCMessageEncoder ARAIPCMessageEncoder;
#endif
//! @}


//! Message Decoder
//! @{
#if defined(__cplusplus)
class ARAIPCMessageDecoder
{
public:
    virtual ~ARAIPCMessageDecoder () = default;

    //! number types
    //! The "size" variant will also be used for the pointer-sized ARA (host) refs.
    //! Will return false and set argValue to 0 if key not found.
    //@{
    virtual bool readInt32 (ARAIPCMessageKey argKey, int32_t* argValue) const = 0;
    virtual bool readInt64 (ARAIPCMessageKey argKey, int64_t* argValue) const = 0;
    virtual bool readSize (ARAIPCMessageKey argKey, size_t* argValue) const = 0;
    virtual bool readFloat (ARAIPCMessageKey argKey, float* argValue) const = 0;
    virtual bool readDouble (ARAIPCMessageKey argKey, double* argValue) const = 0;
    //@}

    //! UTF8-encoded C strings
    //! Will return false and set argValue to NULL if key not found.
    //! Note that received string data is only valid as long as the message that
    //! provided them is alive.
    virtual bool readString (ARAIPCMessageKey argKey, const char ** argValue) const = 0;

    //! raw bytes
    //! first query size, then provide a buffer large enough to copy the bytes to.
    //! readBytesSize () will return false and set argSize to 0 if key not found.
    //@{
    virtual bool readBytesSize (ARAIPCMessageKey argKey, size_t* argSize) const = 0;
    virtual void readBytes (ARAIPCMessageKey argKey, uint8_t* argValue) const = 0;
    //@}

    //! sub-messages to decode compound types
    //! returns nullptr if key not found or if the value for the key is not representing a sub-message
    //! The caller is responsible for deleting the encoder after use.
    virtual ARAIPCMessageDecoder* readSubMessage (ARAIPCMessageKey argKey) const = 0;
};
#else
typedef struct ARAIPCMessageDecoder ARAIPCMessageDecoder;
#endif
//! @}



//! Message Sender: gateway for sending messages
//! @{
#if defined(__cplusplus)
class ARAIPCMessageSender
{
public:
    //! Reply Handler: a function passed to sendMessage () that is called to process the reply to a message
    //! decoder will be nullptr if incoming message was empty
    typedef void (ARA_CALL *ReplyHandler) (const ARAIPCMessageDecoder* decoder, void* userData);

    virtual ~ARAIPCMessageSender() = default;

    //! generate an encoder to encode a new message
    //! An encoder can be reused if the same message is sent several times,
    //! but it must not be modified after sending.
    //! The caller is responsible for deleting the encoder after use.
    virtual ARAIPCMessageEncoder* createEncoder () = 0;

    //! send an encoded messages to the receiving process
    //! If an empty reply ("void") is expected, the replyHandler should be nullptr.
    //! This method can be called from any thread, concurrent calls will be serialized.
    //! The calling thread will be blocked until the receiver has processed the message and
    //! returned a (potentially empty) reply, which will be forwarded to the replyHandler.
    virtual void sendMessage (ARAIPCMessageID messageID, ARAIPCMessageEncoder* encoder,
                              ReplyHandler replyHandler, void* replyHandlerUserData) = 0;

    //! indicate byte order mismatch between sending and receiving machine
    virtual bool receiverEndianessMatches () = 0;
};
#else
typedef struct ARAIPCMessageSender ARAIPCMessageSender;
#endif
//! @}


//! Companion API: opaque encapsulation
//! @{
//! to keep the IPC decoupled from the Companion API in use, the IPC code uses an opaque token to represent a plug-in instance
typedef size_t ARAIPCPlugInInstanceRef;
//! @}


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif


#endif // ARA_ENABLE_IPC

//! @} ARA_Library_IPC

#endif // ARAIPC_h
