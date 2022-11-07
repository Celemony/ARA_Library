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
typedef struct ARAIPCMessageEncoderImplementation * ARAIPCMessageEncoderRef;

typedef struct ARAIPCMessageEncoderInterface
{
    //! destructor
    void (ARA_CALL *destroyEncoder) (ARAIPCMessageEncoderRef encoderRef);

    //! number types
    //! The size variant will also be used for the pointer-sized ARA (host) refs.
    //@{
    void (ARA_CALL *appendInt32) (ARAIPCMessageEncoderRef messageEncoderRef, ARAIPCMessageKey argKey, int32_t argValue);
    void (ARA_CALL *appendInt64) (ARAIPCMessageEncoderRef messageEncoderRef, ARAIPCMessageKey argKey, int64_t argValue);
    void (ARA_CALL *appendSize) (ARAIPCMessageEncoderRef messageEncoderRef, ARAIPCMessageKey argKey, size_t argValue);
    void (ARA_CALL *appendFloat) (ARAIPCMessageEncoderRef messageEncoderRef, ARAIPCMessageKey argKey, float argValue);
    void (ARA_CALL *appendDouble) (ARAIPCMessageEncoderRef messageEncoderRef, ARAIPCMessageKey argKey, double argValue);
    //@}

    //! UTF8-encoded C strings
    void (ARA_CALL *appendString) (ARAIPCMessageEncoderRef messageEncoderRef, ARAIPCMessageKey argKey, const char * argValue);

    //! raw bytes
    //! As optimization, disable copying if the memory containing the bytes stays
    //! alive&unchanged until the message has been sent.
    void (ARA_CALL *appendBytes) (ARAIPCMessageEncoderRef messageEncoderRef, ARAIPCMessageKey argKey, const uint8_t * argValue, size_t argSize, bool copy);

    //! sub-messages to encode compound types
    //! The caller is responsible for deleting the encoder after use.
    ARAIPCMessageEncoderRef (ARA_CALL *appendSubMessage) (ARAIPCMessageEncoderRef messageEncoderRef, ARAIPCMessageKey argKey);
} ARAIPCMessageEncoderInterface;

typedef struct ARAIPCMessageEncoder
{
    ARAIPCMessageEncoderRef ref;
    const ARAIPCMessageEncoderInterface * methods;  // this preferably would be called "interface", but there's a system-defined macro in Windows with that name...
} ARAIPCMessageEncoder;
//! @}


//! Message Decoder
//! @{
typedef struct ARAIPCMessageDecoderImplementation * ARAIPCMessageDecoderRef;

typedef struct ARAIPCMessageDecoderInterface
{
    //! destructor
    void (ARA_CALL *destroyDecoder) (ARAIPCMessageDecoderRef messageDecoderRef);

    //! only for debugging/validation: test if the message contains any key/value pairs
    bool (ARA_CALL *isEmpty) (ARAIPCMessageDecoderRef messageDecoderRef);

    //! number types
    //! The size variant will also be used for the pointer-sized ARA (host) refs.
    //! Will return false and set argValue to 0 if key not found.
    //@{
    bool (ARA_CALL *readInt32) (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey, int32_t * argValue);
    bool (ARA_CALL *readInt64) (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey, int64_t * argValue);
    bool (ARA_CALL *readSize) (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey, size_t * argValue);
    bool (ARA_CALL *readFloat) (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey, float * argValue);
    bool (ARA_CALL *readDouble) (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey, double * argValue);
    //@}

    //! UTF8-encoded C strings
    //! Will return false and set argValue to NULL if key not found.
    bool (ARA_CALL *readString) (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey, const char ** argValue);

    //! raw bytes
    //! first query size, then provide a buffer large enough to copy the bytes to.
    //! readBytesSize () will return false and set argSize to 0 if key not found.
    //@{
    bool (ARA_CALL *readBytesSize) (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey, size_t * argSize);
    void (ARA_CALL *readBytes) (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey, uint8_t * argValue);
    //@}

    //! sub-messages to decode compound types
    //! returns nullptr if key not found or if the value for the key is not representing a sub-message
    //! The caller is responsible for deleting the encoder after use.
    ARAIPCMessageDecoderRef (ARA_CALL *readSubMessage) (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey);
} ARAIPCMessageDecoderInterface;

typedef struct ARAIPCMessageDecoder
{
    ARAIPCMessageDecoderRef ref;
    const ARAIPCMessageDecoderInterface * methods;  // this preferably would be called "interface", but there's a system-defined macro in Windows with that name...
} ARAIPCMessageDecoder;
//! @}


//! Message Receiver: a function that receives a message readable through the decoder, optionally creating a reply
//! Not using the replyEncoder will return a valid empty message to the sender (useful for void calls).
//! Depending on the underlying implementation, replyEncoder may be nullptr if no reply has been
//! requested by the sender, but providing a dummy encoder in this case is valid too.
//! The sender thread will be blocked until the (possibly empty) reply has been received.
//! A receive function can be called from any thread, but not concurrently.
typedef void (ARA_CALL *ARAIPCMessageReceiver) (const ARAIPCMessageID messageID, const ARAIPCMessageDecoder decoder, ARAIPCMessageEncoder * replyEncoder);

//! Reply Handler: a function that is called to process the reply to a message
typedef void (ARA_CALL *ARAIPCReplyHandler) (const ARAIPCMessageDecoder decoder, void * userData);

//! Message Sender: gateway for sending messages
//! @{
typedef struct ARAIPCMessageSenderImplementation * ARAIPCMessageSenderRef;

typedef struct ARAIPCMessageSenderInterface
{
    //! generate an encoder to encode a new message
    //! An encoder can be reused if the same message is sent several times,
    //! but it must not be modified after sending.
    //! The caller is responsible for deleting the encoder after use.
    ARAIPCMessageEncoder (ARA_CALL *createEncoder) (ARAIPCMessageSenderRef messageSenderRef);

    //! send function: send message create using the encoder, blocking until a reply has been received.
    //! If an empty reply ("void") is expected, the replyHandler should be nullptr.
    //! A send function can be called from any thread, but not concurrently.
    void (ARA_CALL *sendMessage) (const bool stackable, ARAIPCMessageSenderRef messageSenderRef, ARAIPCMessageID messageID,
                                  const ARAIPCMessageEncoder * encoder, ARAIPCReplyHandler * const replyHandler, void * replyHandlerUserData);

    //! Test if the receiver runs on a different architecture with different endianess.
    bool (ARA_CALL *receiverEndianessMatches) (ARAIPCMessageSenderRef messageSenderRef);
} ARAIPCMessageSenderInterface;

typedef struct ARAIPCMessageSender
{
    ARAIPCMessageSenderRef ref;
    const ARAIPCMessageSenderInterface * methods;   // this preferably would be called "interface", but there's a system-defined macro in Windows with that name...
} ARAIPCMessageSender;
//! @}


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif


#endif // ARA_ENABLE_IPC

//! @} ARA_Library_IPC

#endif // ARAIPC_h
