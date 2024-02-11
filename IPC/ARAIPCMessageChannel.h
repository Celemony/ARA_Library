//------------------------------------------------------------------------------
//! \file       ARAIPCMessageChannel.h
//!             Base class implementation for both the ARA IPC proxy host and plug-in
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

#ifndef ARAIPCMessageChannel_h
#define ARAIPCMessageChannel_h


#include "ARA_Library/IPC/ARAIPC.h"


#if ARA_ENABLE_IPC


#if defined (_WIN32)
    #include <Windows.h>
#elif defined (__APPLE__)
    #include <dispatch/dispatch.h>
#endif

#include <atomic>
#include <thread>


#if defined(__cplusplus)
namespace ARA {
namespace IPC {

class ARAIPCMessageChannel;

//! @addtogroup ARA_Library_IPC
//! @{

//! delegate interface for processing messages received by an IPC message channel
class ARAIPCMessageHandler
{
public:
    virtual ~ARAIPCMessageHandler () = default;

    //! type returned from getDispatchTargetForIncomingTransaction()
#if defined (_WIN32)
    using DispatchTarget = HANDLE;
    friend void APCProcessReceivedMessageFunc (ULONG_PTR parameter);
#elif defined (__APPLE__)
    using DispatchTarget = dispatch_queue_t;
#else
    #error "not yet implemented on this platform"
#endif

    //! IPC channels will call this method to determine which thread should be
    //! used for handling an incoming transaction. Returning nullptr results in the
    //! current thread being used, otherwise the call will be forwarded to the
    //! returned target thread.
    virtual DispatchTarget getDispatchTargetForIncomingTransaction (ARAIPCMessageID messageID) = 0;

    //! IPC channels will call this method from their receive handler
    //! after filtering replies and routing to the correct thread.
    virtual void handleReceivedMessage (ARAIPCMessageChannel* messageChannel,
                                        const ARAIPCMessageID messageID, const ARAIPCMessageDecoder* const decoder,
                                        ARAIPCMessageEncoder* const replyEncoder) = 0;
};


//! IPC message channel: gateway for sending and receiving messages
//! @{
class ARAIPCMessageChannel
{
public:
    virtual ~ARAIPCMessageChannel ();

    //! Reply Handler: a function passed to sendMessage () that is called to process the reply to a message
    //! decoder will be nullptr if incoming message was empty
    typedef void (ARA_CALL *ReplyHandler) (const ARAIPCMessageDecoder* decoder, void* userData);

    //! send an encoded messages to the receiving process
    //! The encoder will be deleted after sending the message.
    //! If an empty reply ("void") is expected, the replyHandler should be nullptr.
    //! This method can be called from any thread, concurrent calls will be serialized.
    //! The calling thread will be blocked until the receiver has processed the message and
    //! returned a (potentially empty) reply, which will be forwarded to the replyHandler.
    void sendMessage (ARAIPCMessageID messageID, ARAIPCMessageEncoder* encoder,
                      ReplyHandler replyHandler, void* replyHandlerUserData);

    //! implemented by subclasses: generate an encoder to encode a new message,
    //! later passed to sendMessage(), which will destroy the encoder after sending
    virtual ARAIPCMessageEncoder* createEncoder () = 0;

    //! implemented by subclasses: indicate byte order mismatch between sending
    //! and receiving machine
    virtual bool receiverEndianessMatches () = 0;

protected:
    explicit ARAIPCMessageChannel (ARAIPCMessageHandler* handler);

    //! called by subclass implementations to route an incoming message to the correct target thread
    //! takes ownership of the decoder and will eventually delete it
    void routeReceivedMessage (ARAIPCMessageID messageID, const ARAIPCMessageDecoder* decoder);

    //! implemented by subclasses to lock the channel for starting a new transaction
    virtual void lockTransaction () = 0;

    //! implemented by subclasses to unlock the channel after concluding a transaction
    virtual void unlockTransaction () = 0;

    //! implemented by subclasses to perform the actual message (or reply) sending
    virtual void _sendMessage (ARAIPCMessageID messageID, ARAIPCMessageEncoder* encoder) = 0;

    //! called by routeReceivedMessage () when a message comes in while some
    //! thread is already sending (or receiving) on the channel
    //! the default implementation sets a signal unless on the same thread
    //! can be overridden by subclasses if the constraints of the underlying
    //! IPC API require further interaction
    //! passes the thread that is currently sending
    virtual void _signalReceivedMessage (std::thread::id activeThread);

    //! called by a sending (active) thread when waiting for a reply (or incoming call)
    //! the default implementation waits for the signal from _signalReceivedMessage ()
    //! unless on the same thread
    //! can be overridden by subclasses if the constraints of the underlying
    //! IPC API require further interaction (such as spinning a runloop)
    virtual void _waitForReceivedMessage ();

private:
    void _handleReceivedMessage (ARAIPCMessageID messageID, const ARAIPCMessageDecoder* decoder);

#if defined (_WIN32)
    friend void APCRouteNewTransactionFunc (ULONG_PTR parameter);
#endif

private:
    ARAIPCMessageHandler* const _handler;

    std::atomic<std::thread::id> _sendingThread {}; // if != {}, a currently sending thread is waiting for a reply (or stacked callback)

    // incoming data is stored in these ivars by the receive handler if a send loop is currently
    // waiting to pick it up - if that send is on a different thread, the semaphore must be used
    ARAIPCMessageID _receivedMessageID { 0 };
    const ARAIPCMessageDecoder* _receivedDecoder { nullptr };
#if defined (_WIN32)
    HANDLE const _receivedMessageSemaphore;
#elif defined (__APPLE__)
    dispatch_semaphore_t const _receivedMessageSemaphore;
#endif
};
//! @}

//! @} ARA_Library_IPC

}   // namespace IPC
}   // namespace ARA

#else
typedef struct ARAIPCMessageChannel ARAIPCMessageChannel;
#endif

#endif // ARA_ENABLE_IPC

#endif // ARAIPCMessageChannel_h