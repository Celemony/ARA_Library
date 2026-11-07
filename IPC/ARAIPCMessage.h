//------------------------------------------------------------------------------
//! \file       ARAIPCMessage.h
//!             Abstractions shared by both the ARA IPC proxy host and plug-in
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


//! @addtogroup ARA_Library_IPC
//! @{

namespace ARA {
namespace IPC {


//! ID type for messages
typedef ARAInt32 MessageID;

//! the implementation reserves this range of IDs, any custom messages must be < kReservedMessageIDRangeStart or >= kReservedMessageIDRangeEnd
constexpr MessageID kReservedMessageIDRangeStart { 1 };
constexpr MessageID kReservedMessageIDRangeEnd { 8*16*16 };


//! Message Encoder
//! @{
class MessageEncoder
{
public:
    virtual ~MessageEncoder () = default;

    //! number types
    //! The "size" variant will also be used for the pointer-sized ARA (host) refs.
    //@{
    virtual void appendInt32 (int32_t argValue) = 0;
    virtual void appendInt64 (int64_t argValue) = 0;
    virtual void appendSize (size_t argValue) = 0;
    virtual void appendFloat (float argValue) = 0;
    virtual void appendDouble (double argValue) = 0;
    //@}

    //! UTF8-encoded C strings
    virtual void appendString (const char* argValue) = 0;

    //! raw bytes
    //! As optimization, disable copying if the memory containing the bytes stays
    //! alive&unchanged until the message has been sent.
    virtual void appendBytes (const uint8_t* argValue, size_t argSize, bool copy) = 0;

    //! sub-messages to encode compound types
    //! The passed in encoder must have the same type as the receiver, i.e. you can only
    //! pass in an IPCXMLMessageEncoder if the receiver is also an IPCXMLMessageEncoder.
    virtual void appendSubMessage (std::unique_ptr<MessageEncoder>&& argValue) = 0;

    //! helper to prepend data to an encoder by creating a new encoder,
    //! adding the data and then the existing encoder.
    virtual std::unique_ptr<MessageEncoder> createEncoder () = 0;
};
//! @}


//! Message Decoder
//! @{
class MessageDecoder
{
public:
    virtual ~MessageDecoder () = default;

    //! number types
    //! The "size" variant will also be used for the pointer-sized ARA (host) refs.
    //! Will return false and set argValue to 0 if decoding fails
    //! (e.g. because archive data is exhausted).
    //@{
    virtual bool readInt32 (int32_t& argValue) const = 0;
    virtual bool readInt64 (int64_t& argValue) const = 0;
    virtual bool readSize (size_t& argValue) const = 0;
    virtual bool readFloat (float& argValue) const = 0;
    virtual bool readDouble (double& argValue) const = 0;
    //@}

    //! UTF8-encoded C strings
    //! Will return false and set argValue to nullptr if decoding fails
    //! (e.g. because archive data is exhausted).
    //! Note that received string pointer is only valid as long as the message that
    //! provided them is alive.
    virtual bool readString (const char*& argValue) const = 0;

    //! raw bytes
    //! First query size, then provide a buffer large enough to copy the bytes to.
    //! readBytesSize () will return false and set argSize to 0 if decoding fails
    //! (e.g. because archive data is exhausted).
    //@{
    virtual bool readBytesSize (size_t& argSize) const = 0;
    virtual void readBytes (uint8_t* argValue) const = 0;
    //@}
};
//! @}


}   // namespace IPC
}   // namespace ARA

//! @} ARA_Library_IPC


#endif // ARA_ENABLE_IPC

#endif // ARAIPCMessage_h
