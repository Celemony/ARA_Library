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
#include <memory>
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


//! delegate function for processing messages received through an IPC connection
//! IPC connections will call this function for incoming messages after
//! filtering replies and routing them to the correct thread.
using MessageHandler = std::function<void (const MessageID messageID, const MessageDecoder* const decoder,
                                           MessageEncoder* const replyEncoder)>;

//! Reply Handler: a function passed to sendMessage() that is called to process the reply to a message
//! decoder will be nullptr if incoming message was empty
using ReplyHandler = std::function<void (const MessageDecoder* decoder)>;


//! IPC message channel: primitive for sending and receiving messages
//! @{
class MessageChannel
{
public:
    virtual ~MessageChannel () = default;

    //! implemented by subclasses to perform the actual message (or reply) sending
    virtual void sendMessage (MessageID messageID, std::unique_ptr<MessageEncoder> && encoder) = 0;

protected:
    //! called by subclasses when messages arrive
    void routeReceivedMessage (MessageID messageID, std::unique_ptr<const MessageDecoder> && decoder)
    {
        ARA_INTERNAL_ASSERT (_receivedMessageRouter);
        _receivedMessageRouter (messageID, std::move (decoder));
    }

private:
    // configuration hook for the MessageDispatcher constructors
    friend class MainThreadMessageDispatcher;
    friend class OtherThreadsMessageDispatcher;
    std::function<void (MessageID, std::unique_ptr<const MessageDecoder> &&)> _receivedMessageRouter {};
};
//! @}


//! IPC connection: client interface for sending and receiving messages,
//! utilizing potentially multiple MessageChannel instances
//! @{
class Connection
{
public:
    using MessageEncoderFactory = std::function<std::unique_ptr<MessageEncoder> ()>;
    using WaitForMessageDelegate = std::function<void ()>;
    Connection (MessageEncoderFactory && messageEncoderFactory, MessageHandler && messageHandler,
                bool receiverEndianessMatches, WaitForMessageDelegate && waitForMessageDelegate = {});
    ~Connection ();

    //! set the message channel for all main thread communication
    //! Must be done before sending or receiving the first message on any channel.
    void setMainThreadChannel (std::unique_ptr<MessageChannel> && messageChannel);

    //! set the message channel for all non-main thread communication
    //! Must be done before sending or receiving the first message on any channel.
    void setOtherThreadsChannel (std::unique_ptr<MessageChannel> && messageChannel);

    const MessageHandler& getMessageHandler () const { return _messageHandler; }

    //! send an encoded messages to the receiving process
    //! If an empty reply ("void") is expected, the replyHandler shall be omitted.
    //! This method can be called from any thread, concurrent calls will be serialized.
    //! The calling thread will be blocked until the receiver has processed the message and
    //! returned a (potentially empty) reply, which will be forwarded to the replyHandler.
    void sendMessage (MessageID messageID, std::unique_ptr<MessageEncoder> && encoder, ReplyHandler && replyHandler = {});

    //! message encoder factory for users of the connection
    std::unique_ptr<MessageEncoder> createEncoder () { return _messageEncoderFactory (); }

    //! indicate byte order mismatch between sending and receiving machine
    bool receiverEndianessMatches () const { return _receiverEndianessMatches; }

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

    //! spins message processing on the creation thread in case the thread is blocked by some outer loop
    void processPendingMessageOnCreationThreadIfNeeded ();

    //! message dispatcher need to call this when routing a message to the creation thread
    //! in order to wake it up
    void signalMesssageReceived ();

    // internal API: debug output helper
    static void _setDebugMessageHint (bool isHost);

#if defined (__APPLE__)
private:
    static void _performRunLoopSource (void* info);
#endif

private:
    const MessageEncoderFactory _messageEncoderFactory;
    const MessageHandler _messageHandler;
    const bool _receiverEndianessMatches;
    const WaitForMessageDelegate _waitForMessageDelegate;
    void* const _waitForMessageSemaphore;       // concrete type is platform-dependent
    std::unique_ptr<MainThreadMessageDispatcher> _mainThreadDispatcher {};
    std::unique_ptr<OtherThreadsMessageDispatcher> _otherThreadsDispatcher {};
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

}   // namespace IPC
}   // namespace ARA

//! @} ARA_Library_IPC


#endif // ARA_ENABLE_IPC

#endif // ARAIPCConnection_h
