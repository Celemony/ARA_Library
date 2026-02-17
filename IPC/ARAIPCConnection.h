//------------------------------------------------------------------------------
//! \file       ARAIPCConnection.h
//!             Connection to another process to pass messages to/receive messages from it
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

#ifndef ARAIPCConnection_h
#define ARAIPCConnection_h

#include "ARA_Library/IPC/ARAIPCMessage.h"
#include "ARA_Library/Debug/ARADebug.h"

#if ARA_ENABLE_IPC


#if defined (_WIN32)
    #include <Windows.h>
#elif defined (__APPLE__)
    #include <CoreFoundation/CFRunLoop.h>
#endif

#include <functional>
#include <mutex>
#include <thread>
#include <queue>
#include <vector>


//! @addtogroup ARA_Library_IPC
//! @{

namespace ARA {
namespace IPC {


class MessageChannel;
class MessageDispatcher;
class MainThreadMessageDispatcher;
class OtherThreadsMessageDispatcher;


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


//! IPC message channel: primitive for sending and receiving messages
//! @{
class MessageChannel
{
public:
    virtual ~MessageChannel () = default;

    //! implemented by subclasses to perform the actual message (or reply) sending
    virtual void sendMessage (MessageID messageID, MessageEncoder* encoder) = 0;

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


//! IPC connection: gateway for sending and receiving messages,
//! utilizing potentially multiple MessageChannel instances
//! @{
class Connection
{
public:
    using WaitForMessageDelegate = void (*) (void* delegateUserData);
    explicit Connection (WaitForMessageDelegate waitForMessageDelegate, void* delegateUserData);
    virtual ~Connection ();

    //! set the message channel for all main thread communication
    //! Must be done before sending or receiving the first message on any channel.
    //! The connection takes ownership of the channel and deletes them upon teardown.
    void setMainThreadChannel (MessageChannel* messageChannel);

    MainThreadMessageDispatcher* getMainThreadDispatcher () const { return _mainThreadDispatcher; }

    //! set the message channel for all non-main thread communication
    //! Must be done before sending or receiving the first message on any channel.
    //! The connection takes ownership of the channel and deletes them upon teardown.
    void setOtherThreadsChannel (MessageChannel* messageChannel);

    OtherThreadsMessageDispatcher* getOtherThreadsDispatcher () const { return _otherThreadsDispatcher; }

    //! set the message handler for all communication on all threads
    //! Must be done before sending or receiving the first message on any channel.
    //! The connection does not take ownership of the message handler.
    void setMessageHandler (MessageHandler* messageHandler);

    MessageHandler* getMessageHandler () const { return _messageHandler; }

    //! Reply Handler: a function passed to sendMessage () that is called to process the reply to a message
    //! decoder will be nullptr if incoming message was empty
    using ReplyHandler = void (ARA_CALL *) (const MessageDecoder* decoder, void* userData);

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

    bool wasCreatedOnCurrentThread () const { return std::this_thread::get_id () == _creationThreadID; }

    //! if IPC messages that need to be processed on the creation thread are received on some
    //! other thread due to the design of the underlying IPC APIs, then this dispatch allows them
    //! to forward it to the creation thread
    using DispatchableFunction = std::function<void ()>;
    void dispatchToCreationThread (DispatchableFunction func);

    //! when a message dispatcher blocks the creation thread for some time, it needs to periodically
    //! call this method to let other main thread tasks execute cooperatively
    //! returns true if a message was received, false otherwise
    bool waitForMessageOnCreationThread ();

    //! message dispatcher need to call this when routing a message to the creation thread
    //! in order to wake it up
    void signalMesssageReceived ();

#if defined (__APPLE__)
    static void performRunloopSource (void* info);
#endif

private:
    const WaitForMessageDelegate _waitForMessageDelegate;
    void* const _delegateUserData;
    void* const _waitForMessageSemaphore;   // concrete type is platform-dependent
    MainThreadMessageDispatcher* _mainThreadDispatcher {};
    OtherThreadsMessageDispatcher* _otherThreadsDispatcher {};
    MessageHandler* _messageHandler {};
    std::thread::id const _creationThreadID;
#if defined (_WIN32)
    HANDLE const _creationThreadHandle;
#elif defined (__APPLE__)
    CFRunLoopRef const _creationThreadRunLoop;
    CFRunLoopSourceRef _runloopSource;
    std::queue<DispatchableFunction> _queue;    // \todo instead of locking, use a lockless concurrent queue,
    std::recursive_mutex _mutex;                // eg this one: https://github.com/hogliux/farbot
#else
    #error "not yet implemented on this platform"
#endif
};
//! @}


//! IPC message dispatcher: handles threading restrictions for MessageChannel access
//! @{

//! abstract base class
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
    explicit MessageDispatcher (Connection* connection, MessageChannel* messageChannel);
    virtual ~MessageDispatcher ();

    Connection* getConnection () const { return  _connection; }
    MessageChannel* getMessageChannel () const { return  _messageChannel; }

    //! send an encoded messages to the receiving process
    //! The encoder will be deleted after sending the message.
    //! If an empty reply ("void") is expected, the replyHandler should be nullptr.
    //! This method can be called from any thread, concurrent calls will be serialized.
    //! The calling thread will be blocked until the receiver has processed the message and
    //! returned a (potentially empty) reply, which will be forwarded to the replyHandler.
    virtual void sendMessage (MessageID messageID, MessageEncoder* encoder,
                              Connection::ReplyHandler replyHandler, void* replyHandlerUserData) = 0;

    //! route an incoming message to the correct target thread
    //! takes ownership of the decoder and will eventually delete it
    virtual void routeReceivedMessage (MessageID messageID, const MessageDecoder* decoder) = 0;

// protected: this does not work with thread_local...
    struct PendingReplyHandler
    {
        Connection::ReplyHandler _replyHandler;
        void* _replyHandlerUserData;
        const PendingReplyHandler* _prevPendingReplyHandler;
    };

    static bool isReply (MessageID messageID) { return messageID == 0; }

protected:
    void _sendMessage (MessageID messageID, MessageEncoder* encoder);
    bool _waitForMessage ();
    void _handleReply (const MessageDecoder* decoder, Connection::ReplyHandler replyHandler, void* replyHandlerUserData);
    MessageEncoder* _handleReceivedMessage (MessageID messageID, const MessageDecoder* decoder);

    static ThreadRef _getCurrentThread ();

private:
    Connection* const _connection;
    MessageChannel* const _messageChannel;
};

//! single-threaded variant for main thread communication only
class MainThreadMessageDispatcher : public MessageDispatcher
{
public:
    using MessageDispatcher::MessageDispatcher;

    void sendMessage (MessageID messageID, MessageEncoder* encoder,
                      Connection::ReplyHandler replyHandler, void* replyHandlerUserData) override;

    void routeReceivedMessage (MessageID messageID, const MessageDecoder* decoder) override;

    void processPendingMessageIfNeeded ();

private:
// \todo this is not allowed for some reason, so we must cast at every use of _noPendingMessageDecoder...
//  static constexpr auto _noPendingMessageDecoder { reinterpret_cast<const MessageDecoder*> (static_cast<intptr_t> (-1)) };
    static constexpr auto _noPendingMessageDecoder { static_cast<intptr_t> (-1) };
    MessageID _pendingMessageID { 0 };  // read/write _pendingMessageDecoder with proper barrier before/after reading/writing this
    std::atomic<const MessageDecoder*> _pendingMessageDecoder { reinterpret_cast<const MessageDecoder*> (_noPendingMessageDecoder) };

    const PendingReplyHandler* _pendingReplyHandler { nullptr };
};

//! multi-threaded variant for all non-main thread communication
class OtherThreadsMessageDispatcher : public MessageDispatcher
{
public:
    using MessageDispatcher::MessageDispatcher;

    void sendMessage (MessageID messageID, MessageEncoder* encoder,
                      Connection::ReplyHandler replyHandler, void* replyHandlerUserData) override;

    void routeReceivedMessage (MessageID messageID, const MessageDecoder* decoder) override;

private:
    struct RoutedMessage
    {
        MessageID _messageID { 0 };
        const MessageDecoder* _decoder { nullptr };
        ThreadRef _targetThread { _invalidThread };
    };
    RoutedMessage* _getRoutedMessageForThread (ThreadRef thread);

    void _processReceivedMessage (MessageID messageID, const MessageDecoder* decoder);

private:
    // incoming data is stored in _routedMessages by the receive handler for the
    // sending threads waiting to pick it up (signalled via _routeReceiveCondition)
    std::condition_variable _routeReceiveCondition;
    std::vector<RoutedMessage> _routedMessages { 12 };  // we shouldn't use more than a handful of threads concurrently for the IPC
    std::mutex _sendLock;
    std::mutex _routeLock;
};


}   // namespace IPC
}   // namespace ARA

//! @} ARA_Library_IPC


#endif // ARA_ENABLE_IPC

#endif // ARAIPCConnection_h
