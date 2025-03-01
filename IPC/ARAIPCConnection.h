//------------------------------------------------------------------------------
//! \file       ARAIPCConnection.h
//!             Connection to another process to pass messages to/receive messages from it
//! \project    ARA SDK Library
//! \copyright  Copyright (c) 2021-2025, Celemony Software GmbH, All Rights Reserved.
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

#ifndef ARAIPCConnection_h
#define ARAIPCConnection_h

#include "ARA_Library/IPC/ARAIPCMessage.h"
#include "ARA_Library/Debug/ARADebug.h"

#if ARA_ENABLE_IPC

#include <mutex>
#include <thread>
#include <vector>


//! @addtogroup ARA_Library_IPC
//! @{

namespace ARA {
namespace IPC {


class MessageChannel;
class MessageDispatcher;


#if ARA_ENABLE_DEBUG_OUTPUT && 0
    #define ARA_IPC_LOG(...) ARA_LOG ("ARA IPC " __VA_ARGS__)
#else
    #define ARA_IPC_LOG(...) ((void) 0)
#endif


//! delegate interface for processing messages received through an IPC connection
class MessageHandler
{
public:
    virtual ~MessageHandler () = default;

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
    explicit Connection ();

public:
    virtual ~Connection ();

    //! set the message channel for all main thread communication
    //! Must be done before sending or receiving the first message on any channel.
    //! The connection takes ownership of the channels and deletes them upon teardown.
    void setMainThreadChannel (MessageChannel* messageChannel);

    //! set the message channel for all non-main thread communication
    //! Must be done before sending or receiving the first message on any channel.
    //! The connection takes ownership of the channels and deletes them upon teardown.
    void setOtherThreadsChannel (MessageChannel* messageChannel);

    //! set the message handler for all communication on all threads
    //! Must be done before sending or receiving the first message on any channel.
    //! The connection does not take ownership of the message handler.
    void setMessageHandler (MessageHandler* messageHandler);

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

    MessageHandler* getMessageHandler () { return _messageHandler; }

    static bool currentThreadActsAsMainThread ();

private:
    MessageDispatcher* _mainDispatcher {};
    MessageDispatcher* _otherDispatcher {};
    MessageHandler* _messageHandler {};
};
//! @}


//! IPC message dispatcher: handles threading restrictions for MessageChannel access
//! @{
class MessageDispatcher
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
    //! the dispatcher takes ownership of the channel and will delete it upon teardown
    explicit MessageDispatcher (Connection* connection, MessageChannel* messageChannel, bool singleThreaded);
    virtual ~MessageDispatcher ();

    //! send an encoded messages to the receiving process
    //! The encoder will be deleted after sending the message.
    //! If an empty reply ("void") is expected, the replyHandler should be nullptr.
    //! This method can be called from any thread, concurrent calls will be serialized.
    //! The calling thread will be blocked until the receiver has processed the message and
    //! returned a (potentially empty) reply, which will be forwarded to the replyHandler.
    void sendMessage (MessageID messageID, MessageEncoder* encoder,
                      Connection::ReplyHandler replyHandler, void* replyHandlerUserData);

    //! route an incoming message to the correct target thread
    //! takes ownership of the decoder and will eventually delete it
    void routeReceivedMessage (MessageID messageID, const MessageDecoder* decoder);

private:
    void _handleReceivedMessage (MessageID messageID, const MessageDecoder* decoder);
    void _handleReply (const MessageDecoder* decoder, Connection::ReplyHandler replyHandler, void* replyHandlerUserData);
    void _routeMessageToThread (MessageID messageID, const MessageDecoder* decoder, ThreadRef targetThread);
    void _addRecursionThread ();

    struct RoutedMessage
    {
        MessageID _messageID { 0 };
        const MessageDecoder* _decoder { nullptr };
        ThreadRef _targetThread { _invalidThread };
    };
    RoutedMessage* _getRoutedMessageForThread (ThreadRef thread);

    bool _isSingleThreaded () { return _sendLock == nullptr; }

    static ThreadRef _getThreadRefForThreadID (std::thread::id threadID);
    static ThreadRef _getCurrentThreadRef () { return _getThreadRefForThreadID (std::this_thread::get_id ()); }

private:
    Connection* const _connection;
    MessageChannel* const _messageChannel;

    std::mutex* _sendLock {};   // optional, nullptr if channel is only used from a single thread at a time
    std::mutex _routeLock;

    // incoming data is stored in _routedMessages by the receive handler for the
    // sending threads waiting to pick it up (signalled via _routeReceiveCondition)
    std::condition_variable _routeReceiveCondition;
    std::vector<RoutedMessage> _routedMessages;

    size_t _nextRecursionThreadIndex { 0 };
    bool _shutDown { false };
    std::vector<std::thread*> _recursionThreads;
};


//! IPC message channel: primitive for sending and receiving messages
//! @{
class MessageChannel
{
public:
    virtual ~MessageChannel () = default;

    //! implemented by subclasses to perform the actual message (or reply) sending
    virtual void sendMessage (MessageID messageID, MessageEncoder* encoder) = 0;

    //! implemented by subclasses for IPC APIs that require spinning a receive
    //! loop on some thread(s) to indicate that the thread cannot be blocked
    //! while waiting for messages
    virtual bool runsReceiveLoopOnCurrentThread () { return false; }

    //! implemented by subclasses for IPC APIs that require spinning a receive
    //! loop on some thread(s) in order to run the loop until a message was received
    //! and routed/handled
    virtual void loopUntilMessageReceived () {}

    void setMessageDispatcher (MessageDispatcher* messageDispatcher)
    {
        ARA_INTERNAL_ASSERT (_messageDispatcher == nullptr);
        _messageDispatcher = messageDispatcher;
    }

    MessageDispatcher* getMessageDispatcher () const { return _messageDispatcher; }

private:
    MessageDispatcher* _messageDispatcher {};
};
//! @}


}   // namespace IPC
}   // namespace ARA

//! @} ARA_Library_IPC


#endif // ARA_ENABLE_IPC

#endif // ARAIPCConnection_h
