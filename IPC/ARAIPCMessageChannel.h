//------------------------------------------------------------------------------
//! \file       ARAIPCMessageChannel.h
//!             Base class implementation for both the ARA IPC proxy host and plug-in
//!             Typically, this file is not included directly - either ARAIPCProxyHost.h
//!             ARAIPCProxyPlugIn.h will be used instead.
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

#ifndef ARAIPCMessageChannel_h
#define ARAIPCMessageChannel_h

#include "ARA_Library/IPC/ARAIPCMessage.h"

#if ARA_ENABLE_IPC


#if defined (_WIN32)
    #include <Windows.h>
#elif defined (__APPLE__)
    #include <dispatch/dispatch.h>
#endif

#include <mutex>
#include <thread>
#include <vector>


//! @addtogroup ARA_Library_IPC
//! @{

namespace ARA {
namespace IPC {


class MessageChannel;


//! delegate interface for processing messages received through an IPC connection
class MessageHandler
{
public:
    virtual ~MessageHandler () = default;

    //! type returned from getDispatchTargetForIncomingTransaction()
#if defined (_WIN32)
    using DispatchTarget = HANDLE;
    friend void APCProcessReceivedMessageFunc (ULONG_PTR parameter);
#elif defined (__APPLE__)
    using DispatchTarget = dispatch_queue_t;
#else
    #error "not yet implemented on this platform"
#endif

    //! IPC connections will call this method to determine which thread should be
    //! used for handling an incoming transaction. Returning nullptr results in the
    //! current thread being used, otherwise the call will be forwarded to the
    //! returned target thread.
    virtual DispatchTarget getDispatchTargetForIncomingTransaction (MessageID messageID) = 0;

    //! IPC connections will call this method for incoming messages after
    //! after filtering replies and routing them to the correct thread.
    virtual void handleReceivedMessage (const MessageID messageID, const MessageDecoder* const decoder,
                                        MessageEncoder* const replyEncoder) = 0;
};


//! IPC connection: gateway for sending and receiving messages,
//! utilizing potentially multiple MessageChannel instances
//! @{
class Connection
{
protected:
    //! creation requires a message handler, a channel for all main thread
    //! communication and a channel for all communication on other threads
    //! The connections takes ownership of the channels and deletes them upon teardown.
    Connection (MessageHandler* messageHandler, MessageChannel* mainChannel, MessageChannel* otherChannel);

    //! alternate creation when mainChannel and otherThreadsChannel cannot be
    //! provided at construction time
    explicit Connection (MessageHandler* messageHandler);

public:
    virtual ~Connection ();

    //! set the message channel for all main thread communication
    //! Must be done before sending or receiving the first message on any channel.
    //! The connections takes ownership of the channels and deletes them upon teardown.
    void setMainThreadChannel (MessageChannel* mainChannel);

    //! set the message channel for all non-main thread communication
    //! Must be done before sending or receiving the first message on any channel.
    //! The connections takes ownership of the channels and deletes them upon teardown.
    void setOtherThreadsChannel (MessageChannel* otherChannel);

    //! Reply Handler: a function passed to sendMessage () that is called to process the reply to a message
    //! decoder will be nullptr if incoming message was empty
    typedef void (ARA_CALL *ReplyHandler) (const MessageDecoder* decoder, void* userData);

    //! send an encoded messages to the receiving process
    //! The encoder will be deleted after sending the message.
    //! If an empty reply ("void") is expected, the replyHandler should be nullptr.
    //! This method can be called from any thread, concurrent calls will be serialized.
    //! The calling thread will be blocked until the receiver has processed the message and
    //! returned a (potentially empty) reply, which will be forwarded to the replyHandler.
    void sendMessage (MessageID messageID, MessageEncoder* encoder,
                      ReplyHandler replyHandler, void* replyHandlerUserData);

    //! implemented by subclasses: generate an encoder to encode a new message,
    //! later passed to sendMessage(), which will destroy the encoder after sending
    virtual MessageEncoder* createEncoder () = 0;

    //! implemented by subclasses: indicate byte order mismatch between sending
    //! and receiving machine
    virtual bool receiverEndianessMatches () = 0;

protected:
    MessageChannel* getMainChannel () const
    {
        return _mainChannel;
    }

    friend MessageChannel;
    MessageHandler::DispatchTarget getDispatchTargetForIncomingTransaction (MessageID messageID);
    void handleReceivedMessage (const MessageID messageID, const MessageDecoder* const decoder,
                                MessageEncoder* const replyEncoder);

private:
    MessageHandler* const _messageHandler;
    MessageChannel* _mainChannel {};
    MessageChannel* _otherChannel {};
    std::thread::id const _creationThreadID;
};
//! @}


//! IPC message channel: primitive for sending and receiving messages
//! @{
class MessageChannel
{
public:     // needs to be public for thread-local variables (which cannot be class members)
#if defined (_WIN32)
    using ThreadRef = int32_t;
#elif defined (__APPLE__)
    using ThreadRef = size_t;
#else
    #error "not yet implemented on this platform"
#endif
    static constexpr ThreadRef _invalidThread { 0 };

public:
    virtual ~MessageChannel () = default;

    //! send an encoded messages to the receiving process
    //! The encoder will be deleted after sending the message.
    //! If an empty reply ("void") is expected, the replyHandler should be nullptr.
    //! This method can be called from any thread, concurrent calls will be serialized.
    //! The calling thread will be blocked until the receiver has processed the message and
    //! returned a (potentially empty) reply, which will be forwarded to the replyHandler.
    void sendMessage (MessageID messageID, MessageEncoder* encoder,
                      Connection::ReplyHandler replyHandler, void* replyHandlerUserData);

protected:
    explicit MessageChannel (Connection* connection);

    //! called by subclass implementations to route an incoming message to the correct target thread
    //! takes ownership of the decoder and will eventually delete it
    void routeReceivedMessage (MessageID messageID, const MessageDecoder* decoder);

    //! implemented by subclasses to perform the actual message (or reply) sending
    virtual void _sendMessage (MessageID messageID, MessageEncoder* encoder) = 0;

    //! implemented by subclasses for IPC APIs that require spinning a receive
    //! loop on some thread(s) to indicate that the thread cannot be blocked
    //! while waiting for messages
    virtual bool runsReceiveLoopOnCurrentThread () { return false; }

    //! implemented by subclasses for IPC APIs that require spinning a receive
    //! loop on some thread(s) in order to run the loop until a message was received
    //! and routed/handled
    virtual void loopUntilMessageReceived () {}

private:
    void _handleReceivedMessage (MessageID messageID, const MessageDecoder* decoder);
    void _handleReply (const MessageDecoder* decoder, Connection::ReplyHandler replyHandler, void* replyHandlerUserData);

    static ThreadRef _getCurrentThread ();

#if defined (_WIN32)
    friend void APCRouteNewTransactionFunc (ULONG_PTR parameter);
#endif

    struct RoutedMessage
    {
        MessageID _messageID { 0 };
        const MessageDecoder* _decoder { nullptr };
        ThreadRef _targetThread { _invalidThread };
    };

    RoutedMessage* _getRoutedMessageForThread (ThreadRef thread);

private:
    Connection* const _connection;

    std::mutex _lock;

    // incoming data is stored in _routedMessages by the receive handler for the
    // sending threads waiting to pick it up (signaled via _routeReceiveCondition)
    std::condition_variable _routeReceiveCondition;
    std::vector<RoutedMessage> _routedMessages;
};
//! @}


}   // namespace IPC
}   // namespace ARA

//! @} ARA_Library_IPC


#endif // ARA_ENABLE_IPC

#endif // ARAIPCMessageChannel_h
