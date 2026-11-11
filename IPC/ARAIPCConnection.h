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

#if ARA_ENABLE_IPC


#include "ARA_Library/Debug/ARADebug.h"
#include "ARA_Library/Utilities/ARAFunctionDispatcher.h"

#include <functional>
#include <memory>
#include <thread>


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
using MessageHandler = std::function<void (const MessageID messageID, const IncomingMessage* const message,
                                           OutgoingMessage* const reply)>;

//! Arguments Provider: callback passed to sendMessage() that stores the arguments into the outgoing message
using ArgumentsProvider = std::function<void (OutgoingMessage* message)>;

//! Reply Handler: callback passed to sendMessage() that processed the incoming reply to the message
using ReplyHandler = std::function<void (const IncomingMessage* message)>;


//! IPC message channel: primitive for sending and receiving messages
//! @{
class MessageChannel
{
public:
    virtual ~MessageChannel () = default;

    //! implemented by subclasses to perform the actual message (or reply) sending
    virtual void sendMessage (std::unique_ptr<OutgoingMessage>&& message) = 0;

    //! implemented by subclasses to inform the message dispatching code of the threading requirements
    virtual bool receivesMessagesOnCurrentThread () = 0;

    //! implemented by subclasses to receive messages if receivesMessagesOnCurrentThread() returns true
    //! waits up to the specified time in seconds for an incoming event and processes it
    //! if timeout is 0, it returns immediately if no message is pending
    //! returns true if an event was processed during the call
    //! note that waiting may trigger execution of functions that have been dispatched via
    //! FunctionDispatcher::dispatchToCreationThread()
    virtual bool waitForMessageOnCurrentThread (ARATimeDuration timeout) = 0;

protected:
    //! called by subclasses when messages arrive
    void routeReceivedMessage (std::unique_ptr<const IncomingMessage>&& message)
    {
        ARA_INTERNAL_ASSERT (_receivedMessageRouter);
        _receivedMessageRouter (std::move (message));
    }

private:
    // configuration hook for the MessageDispatcher constructors
    friend class MainThreadMessageDispatcher;
    friend class OtherThreadsMessageDispatcher;
    std::function<void (std::unique_ptr<const IncomingMessage>&&)> _receivedMessageRouter {};
};
//! @}


//! IPC connection: client interface for sending and receiving messages,
//! utilizing potentially multiple MessageChannel instances
//! @{
class Connection
{
public:
    using OutgoingMessageFactory = std::function<std::unique_ptr<OutgoingMessage> ()>;
    using CreationThreadDispatcher = std::function<void (std::function<void ()>)>;

    Connection (OutgoingMessageFactory&& outgoingMessageFactory, MessageHandler&& messageHandler,
                CreationThreadDispatcher&& creationThreadDispatcher,
                bool receiverEndiannessMatches);
    ~Connection ();

    //! set the message channel for all main thread communication
    //! Must be done before sending or receiving the first message on any channel.
    void setMainThreadChannel (std::unique_ptr<MessageChannel>&& messageChannel);

    //! set the message channel for all non-main thread communication
    //! Must be done before sending or receiving the first message on any channel.
    void setOtherThreadsChannel (std::unique_ptr<MessageChannel>&& messageChannel);

    const MessageHandler& getMessageHandler () const { return _messageHandler; }

    //! encode and send a messages to the receiving process
    //! The connection will provide an empty outgoing message and pass it to the argumentsProvider
    //! so it can store the arguments into it.
    //! This method can be called from any thread, concurrent calls will be serialized.
    //! The calling thread will be blocked until the receiver has processed the message and
    //! returned a (potentially empty) reply, which will be forwarded to the replyHandler.
    //! If an empty reply ("void") is expected, the replyHandler shall be omitted.
    void sendMessage (MessageID messageID, ArgumentsProvider&& argumentsProvider, ReplyHandler&& replyHandler = {});

    //! indicate byte order mismatch between sending and receiving machine
    bool receiverEndiannessMatches () const { return _receiverEndiannessMatches; }

    bool wasCreatedOnCurrentThread () const { return std::this_thread::get_id () == _creationThreadID; }

    //! if IPC messages that need to be processed on the creation thread are received on some
    //! other thread due to the design of the underlying IPC APIs, then this dispatch allows
    //! to forward their processing to the creation thread
    void dispatchToCreationThread (std::function<void ()> func)
    {
        _creationThreadDispatcher (func);
    }

    //! spins message processing on the creation thread in case the thread is blocked by some outer loop
    //! will return immediately if timeout == 0 and no message pending
    void processPendingMessageOnCreationThreadIfNeeded (ARATimeDuration timeout);

    // internal API: called internally when sending messages and by message dispatchers when creating replies
    std::unique_ptr<OutgoingMessage> _createOutgoingMessage ();

    // internal API: debug output helper
    static void _setDebugMessageHint (bool isHost);

private:
    const OutgoingMessageFactory _outgoingMessageFactory;
    const MessageHandler _messageHandler;
    const CreationThreadDispatcher _creationThreadDispatcher;
    const bool _receiverEndiannessMatches;
    std::unique_ptr<MainThreadMessageDispatcher> _mainThreadDispatcher {};
    std::unique_ptr<OtherThreadsMessageDispatcher> _otherThreadsDispatcher {};
    std::thread::id const _creationThreadID;
};

}   // namespace IPC
}   // namespace ARA

//! @} ARA_Library_IPC


#endif // ARA_ENABLE_IPC

#endif // ARAIPCConnection_h
