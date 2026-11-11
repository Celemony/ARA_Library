//------------------------------------------------------------------------------
//! \file       ARAIPCConnection.cpp
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

#include "ARAIPCConnection.h"


#if ARA_ENABLE_IPC


#include "ARA_Library/Debug/ARADebug.h"
#include "ARA_Library/IPC/ARAIPCEncoding.h"
#include "ARA_Library/Utilities/ARAStdVectorUtilities.h"

#include <condition_variable>
#include <mutex>
#include <utility>
#include <vector>


#if ARA_ENABLE_DEBUG_OUTPUT && 0
    #define ARA_IPC_LOG(...) ARA_LOG ("ARA IPC " __VA_ARGS__)
    #define ARA_IPC_DECODE_MESSAGE_FORMAT " %i=%s "
    #define ARA_IPC_DECODE_SENT_MESSAGE_ARGS(messageID) messageID, (_debugAsHost) ? decodeHostMessageID (messageID) : decodePlugInMessageID (messageID)
    #define ARA_IPC_DECODE_RECEIVED_MESSAGE_ARGS(messageID) messageID, (!_debugAsHost) ? decodeHostMessageID (messageID) : decodePlugInMessageID (messageID)
    #define ARA_IPC_LABEL_THREAD_FORMAT " %sthread %p"
    #define ARA_IPC_LABEL_THREAD_ARGS (getConnection ()->wasCreatedOnCurrentThread ()) ? "creation " : "other ", MessageDispatcher::getCurrentThread ()
#else
    #define ARA_IPC_LOG(...) ((void) 0)
#endif


// ARA IPC design overview
//
// ARA API calls can be stacked multiple times.
// For example, when the plug-in has completed analyzing an audio source, it will
// inform the host about the updated content information the next time the host
// calls notifyModelUpdates() by invoking notifyAudioSourceContentChanged().
// From that method, the host now queries isAudioSourceContentAvailable() for the
// relevant content types and calls createAudioSourceContentReader() etc. to read
// the relevant content data.
// Such a stack of multiple messages is referred to as a "transaction" here.
//
// However, many IPC APIs including Audio Unit message channels cannot be stacked,
// i.e. each message has to completely be processed before the next one can be sent.
// Therefore, each ARA API calls needs to be split into two IPC messages:
// the actual call message that does not return any result just yet, and a matching
// reply message that returns the result (even if it is void, because the caller
// needs to know when the call has completed).
// After sending the actual call message, the sender loops while listening for
// incoming messages, which might either be a stacked callback that is processed
// accordingly, or the reply message which ends the loop.
//
// In this loop, extra efforts might be necessary to handle the IPC threading:
// When e.g. using Audio Unit AUMessageChannel, Grand Central Dispatch will deliver
// the incoming IPC messages on some undefined GDC worker thread, requiring a dispatch
// from the receiving thread to the target thread. This is implemented via
// a condition variable that the receive thread awakes when a message comes in.
// Instead of looping, a sending thread will wait on this condition for the reply.
//
// While most ARA communication is happening on the main thread, there are
// several calls that may be made from other threads. This poses several challenges
// when tunnelling everything through a single(threaded) IPC channel.
//
// First of all, there needs to be some locking mechanism around actually accessing
// the IPC. This adds a dependency between previously independent threads, so both
// hosts and plug-ins will need to carefully evaluate their existing ARA code to check
// for potential deadlocks or priority inversion caused by this.
// To reduce the potential impact of this, the implementation uses two IPC channels
// in parallel: one for all main thread communication (which can work lockless) and
// another one that is used for all other threads.
//
// Further, calls that are using IPC are no longer realtime safe. This means that calls
// like getPlaybackRegionHeadAndTailTime(), which could previously be executed on render
// threads, now need to be moved to other threads. Accordingly, hosts need to cache data
// like the head and tail times from the main thread and update them there whenever
// notifyPlaybackRegionContentChanged() is received.
//
// Another challenge when injecting IPC into the ARA communication is that for each
// message that comes in, an appropriate thread has to be selected to process it.
// When a new transaction is initiated, the implementation checks whether this is happening
// on the main thread or on any other thread, and chooses the appropriate channel accordingly.
// On the receiving side, calls coming in on the main thread channel are forwarded to the main
// thread (unless the receive code already runs there), and for the other threads the code is
// executed directly on the receiving thread.
// Because there's no mechanism to synchronize thread priorities between the host and the
// plug-in for the non-main-threads, plug-ins will no longer be able to rely on their thread
// priority configuration with respect to ARA calls. For this reason, it is recommended to
// restrict these calls to audio sample reading, and order these explicitly before making them.
//
// Replies or callbacks made when processing an IPC call are routed back to the originating
// thread in the sender. For non-main-thread communication, this is done by adding a token when
// sending a message which identifies the sending thread, and this token passed back alongside
// the reply or stacked callback to allow for proper dispatching from the IPC receive thread to
// the thread that initiated the transaction.
// Since the actual ARA code is agnostic to IPC being used, the receiving side uses
// thread local storage to make the sender's thread token available for all stacked calls
// that the ARA code might make in response to the message.
//
// Note that there is a crucial difference between the dispatch of a new transaction and
// the dispatch of any follow-up replies or callbacks in the transaction: the initial message
// is dispatched to a thread that is potentially executing other code as well in some form of
// run loop, whereas the follow-ups will be dispatched to a thread that is currently blocking
// inside ARA code.


namespace ARA {
namespace IPC {


static bool _debugAsHost {};

//------------------------------------------------------------------------------

class MessageDispatcher
{
protected:
#if defined (_WIN32)
    using ThreadRef = int32_t;
#elif defined (__APPLE__)
    using ThreadRef = size_t;
#else
    #error "not yet implemented on this platform"
#endif
    static constexpr ThreadRef _invalidThread { 0 };

    static ThreadRef getCurrentThread ();

    struct PendingReplyHandler
    {
        ReplyHandler* const _replyHandler;
        const PendingReplyHandler* _prevPendingReplyHandler;
    };

protected:
    explicit MessageDispatcher (Connection* connection, std::unique_ptr<MessageChannel>&& messageChannel);

    Connection* getConnection () const { return  _connection; }
    MessageChannel* getMessageChannel () const { return  _messageChannel.get (); }

    static bool isReply (MessageID messageID) { return messageID == 0; }

    void _sendMessage (std::unique_ptr<OutgoingMessage>&& message,
                       MessageID messageID /*only for debug output*/);
    void _handleReply (std::unique_ptr<const IncomingMessage>&& message, ReplyHandler&& replyHandler);
    void _handleReceivedMessage (MessageID messageID, std::unique_ptr<const IncomingMessage>&& message, OutgoingMessage* reply);

private:
    Connection* const _connection;
    const std::unique_ptr<MessageChannel> _messageChannel;
};


MessageDispatcher::MessageDispatcher (Connection* connection, std::unique_ptr<MessageChannel>&& messageChannel)
: _connection { connection },
  _messageChannel { std::move (messageChannel) }
{
    static_assert (sizeof (std::thread::id) == sizeof (ThreadRef), "the current implementation relies on a specific thread ID size");
// unfortunately at least in clang std::thread::id's c'tor isn't constexpr
//  static_assert (std::thread::id {} == *reinterpret_cast<const std::thread::id*>(&_invalidThread), "the current implementation relies on invalid thread IDs being 0");
    ARA_INTERNAL_ASSERT (std::thread::id {} == *reinterpret_cast<const std::thread::id*>(&_invalidThread));
}

MessageDispatcher::ThreadRef MessageDispatcher::getCurrentThread ()
{
    const auto thisThread { std::this_thread::get_id () };
    const auto result { *reinterpret_cast<const ThreadRef*> (&thisThread) };
    ARA_INTERNAL_ASSERT (result != _invalidThread);
    return result;
}

void MessageDispatcher::_sendMessage (std::unique_ptr<OutgoingMessage>&& message,
                                      [[maybe_unused]] MessageID messageID)
{
    if (isReply (messageID))
        ARA_IPC_LOG ("replies to message on" ARA_IPC_LABEL_THREAD_FORMAT, ARA_IPC_LABEL_THREAD_ARGS);
    else
        ARA_IPC_LOG ("sends" ARA_IPC_DECODE_MESSAGE_FORMAT "on" ARA_IPC_LABEL_THREAD_FORMAT "%s",
                     ARA_IPC_DECODE_SENT_MESSAGE_ARGS (messageID),  ARA_IPC_LABEL_THREAD_ARGS);

    // \todo potential optimization: instead of discarding the message after sending it,
    //       we could recycle it for the next message being sent, minimizing allocations.
    _messageChannel->sendMessage (std::move (message));
}

void MessageDispatcher::_handleReply (std::unique_ptr<const IncomingMessage>&& message, ReplyHandler&& replyHandler)
{
    ARA_IPC_LOG ("handles reply on" ARA_IPC_LABEL_THREAD_FORMAT, ARA_IPC_LABEL_THREAD_ARGS);
    if (replyHandler)
        replyHandler (message.get ());
}

void MessageDispatcher::_handleReceivedMessage (MessageID messageID, std::unique_ptr<const IncomingMessage>&& message, OutgoingMessage* reply)
{
    ARA_INTERNAL_ASSERT (!isReply (messageID));

    ARA_IPC_LOG ("handles" ARA_IPC_DECODE_MESSAGE_FORMAT "on" ARA_IPC_LABEL_THREAD_FORMAT,
                 ARA_IPC_DECODE_RECEIVED_MESSAGE_ARGS (messageID), ARA_IPC_LABEL_THREAD_ARGS);
    
    _connection->getMessageHandler () (messageID, message.get (), reply);
}

//------------------------------------------------------------------------------

// single-threaded variant for main thread communication only
class MainThreadMessageDispatcher : public MessageDispatcher
{
public:
    MainThreadMessageDispatcher (Connection* connection, std::unique_ptr<MessageChannel>&& messageChannel)
    : MessageDispatcher { connection, std::move (messageChannel) }
    {
        getMessageChannel ()->_receivedMessageRouter = [this] (std::unique_ptr<const IncomingMessage>&& message)
                                                              { routeReceivedMessage (std::move (message)); };
    }

    void initializeMessage (OutgoingMessage* message, MessageID messageID);

    void sendMessage (std::unique_ptr<OutgoingMessage>&& message, ReplyHandler&& replyHandler,
                      MessageID messageID /*only for debug output*/);

    void routeReceivedMessage (std::unique_ptr<const IncomingMessage>&& message);

    bool processPendingMessageIfNeeded (ARATimeDuration timeout)
    {
        ARA_INTERNAL_ASSERT (getConnection ()->wasCreatedOnCurrentThread ());

        return getMessageChannel ()->waitForMessageOnCurrentThread (timeout);
    }

protected:
    void processMessage (MessageID messageID, std::unique_ptr<const IncomingMessage>&& message);

private:
    const PendingReplyHandler* _pendingReplyHandler { nullptr };
};


void MainThreadMessageDispatcher::initializeMessage (OutgoingMessage* message, MessageID messageID)
{
    _appendToMessage (message, messageID);
}

void MainThreadMessageDispatcher::sendMessage (std::unique_ptr<OutgoingMessage>&& message,
                                               ReplyHandler&& replyHandler, [[maybe_unused]] MessageID messageID)
{
    const auto previousPendingReplyHandler { _pendingReplyHandler };
    const PendingReplyHandler pendingReplyHandler { &replyHandler, previousPendingReplyHandler };
    _pendingReplyHandler = &pendingReplyHandler;

    _sendMessage (std::move (message), messageID);

    do
    {
        getMessageChannel ()->waitForMessageOnCurrentThread (0.001);
    }
    while (previousPendingReplyHandler != _pendingReplyHandler);
}

void MainThreadMessageDispatcher::processMessage (MessageID messageID, std::unique_ptr<const IncomingMessage>&& message)
{
    ARA_INTERNAL_ASSERT (getConnection ()->wasCreatedOnCurrentThread ());

    if (isReply (messageID))
    {
        ARA_INTERNAL_ASSERT (_pendingReplyHandler != nullptr);
        _handleReply (std::move (message), std::move (*_pendingReplyHandler->_replyHandler));
        _pendingReplyHandler = _pendingReplyHandler->_prevPendingReplyHandler;
    }
    else
    {
        auto reply { getConnection ()->_createOutgoingMessage () };
        initializeMessage (reply.get (), 0);
        _handleReceivedMessage (messageID, std::move (message), reply.get ());

        _sendMessage (std::move (reply), 0);
    }
}

void MainThreadMessageDispatcher::routeReceivedMessage (std::unique_ptr<const IncomingMessage>&& message)
{
    ARA_INTERNAL_ASSERT (message);
    MessageID messageID;
    [[maybe_unused]] auto success { _readFromMessage (message.get (), messageID) };
    ARA_INTERNAL_ASSERT (success);

    if (getConnection ()->wasCreatedOnCurrentThread ())
    {
        if (isReply (messageID))
            ARA_IPC_LOG ("processes reply on" ARA_IPC_LABEL_THREAD_FORMAT, ARA_IPC_LABEL_THREAD_ARGS);
        else
            ARA_IPC_LOG ("processes" ARA_IPC_DECODE_MESSAGE_FORMAT "on" ARA_IPC_LABEL_THREAD_FORMAT,
                         ARA_IPC_DECODE_RECEIVED_MESSAGE_ARGS (messageID), ARA_IPC_LABEL_THREAD_ARGS);

        processMessage (messageID, std::move (message));
    }
    else
    {
        if (isReply (messageID))
            ARA_IPC_LOG ("dispatches reply from" ARA_IPC_LABEL_THREAD_FORMAT " to creation thread", ARA_IPC_LABEL_THREAD_ARGS);
        else
            ARA_IPC_LOG ("dispatches" ARA_IPC_DECODE_MESSAGE_FORMAT "from" ARA_IPC_LABEL_THREAD_FORMAT " to creation thread",
                         ARA_IPC_DECODE_RECEIVED_MESSAGE_ARGS (messageID), ARA_IPC_LABEL_THREAD_ARGS);

        getConnection ()->dispatchToCreationThread ([this, messageID, d { message.release () }] ()
                                                    { processMessage (messageID, std::unique_ptr<const IncomingMessage> (d)); });
    }
}

//------------------------------------------------------------------------------

// multi-threaded variant for all non-main thread communication
// Adds a reference to the sending thread to the message, which is passed back as target thread
// with the reply or any callback made in response to the message. When initially starting the
// transaction, the target thread is unknown, in which case the _invalidThread constant is sent.
class OtherThreadsMessageDispatcher : public MessageDispatcher
{
public:
    OtherThreadsMessageDispatcher (Connection* connection, std::unique_ptr<MessageChannel>&& messageChannel)
    : MessageDispatcher { connection, std::move (messageChannel) }
    {
        getMessageChannel ()->_receivedMessageRouter = [this] (std::unique_ptr<const IncomingMessage>&& message)
                                                              { routeReceivedMessage (std::move (message)); };
    }

    void initializeMessage (OutgoingMessage* message, MessageID messageID);

    void sendMessage (std::unique_ptr<OutgoingMessage>&& message, ReplyHandler&& replyHandler,
                      MessageID messageID /*only for debug output*/);

    void routeReceivedMessage (std::unique_ptr<const IncomingMessage>&& message);

private:
    struct RoutedMessage
    {
        MessageID _messageID { 0 };
        std::unique_ptr<const IncomingMessage> _message;
        ThreadRef _targetThread { _invalidThread };
    };
    RoutedMessage* _getRoutedMessageForThread (ThreadRef thread);

    void _processReceivedMessage (MessageID messageID, std::unique_ptr<const IncomingMessage>&& message);

private:
    // incoming data is stored in _routedMessages by the receive handler for the
    // sending threads waiting to pick it up (signalled via _routeReceiveCondition)
    std::condition_variable _routeReceiveCondition;
    std::vector<RoutedMessage> _routedMessages { 12 };  // we shouldn't use more than a handful of threads concurrently for the IPC
    std::mutex _sendLock;
    std::mutex _routeLock;

    static thread_local ThreadRef _remoteTargetThread;
    static thread_local const PendingReplyHandler* _pendingReplyHandler;
};
thread_local MessageDispatcher::ThreadRef OtherThreadsMessageDispatcher::_remoteTargetThread { MessageDispatcher::_invalidThread };
thread_local const OtherThreadsMessageDispatcher::PendingReplyHandler* OtherThreadsMessageDispatcher::_pendingReplyHandler { nullptr };


void OtherThreadsMessageDispatcher::initializeMessage (OutgoingMessage* message, MessageID messageID)
{
    _appendToMessage (message, messageID);
    _appendToMessage (message, _remoteTargetThread);
    if (!isReply (messageID))
        _appendToMessage (message, getCurrentThread ());    // not needed in replies
}

void OtherThreadsMessageDispatcher::sendMessage (std::unique_ptr<OutgoingMessage>&& message,
                                                 ReplyHandler&& replyHandler, [[maybe_unused]] MessageID messageID)
{
    _sendLock.lock ();
    _sendMessage (std::move (message), messageID);
    _sendLock.unlock ();

    if (getMessageChannel ()->receivesMessagesOnCurrentThread ())
    {
        const auto previousPendingReplyHandler { _pendingReplyHandler };
        const PendingReplyHandler pendingReplyHandler { &replyHandler, previousPendingReplyHandler };
        _pendingReplyHandler = &pendingReplyHandler;
        do
        {
            getMessageChannel ()->waitForMessageOnCurrentThread (0.001);
        } while (_pendingReplyHandler != previousPendingReplyHandler);
    }
    else
    {
        const auto currentThread { getCurrentThread () };
        while (true)
        {
            std::unique_lock <std::mutex> lock { _routeLock };
            _routeReceiveCondition.wait (lock, [this, &currentThread]
                                    { return _getRoutedMessageForThread (currentThread) != nullptr; });
            const auto routedMessage { _getRoutedMessageForThread (currentThread) };
            const auto receivedMessageID { routedMessage->_messageID };
            auto receivedMessage { std::move (routedMessage->_message) };
            routedMessage->_targetThread = _invalidThread;
            lock.unlock ();

            if (isReply (receivedMessageID))
            {
                _handleReply (std::move (receivedMessage), std::move (replyHandler));
                break;
            }
            else
            {
                _processReceivedMessage (receivedMessageID, std::move (receivedMessage));
            }
        }
    }
}

OtherThreadsMessageDispatcher::RoutedMessage* OtherThreadsMessageDispatcher::_getRoutedMessageForThread (ThreadRef thread)
{
    for (auto& message : _routedMessages)
    {
        if (message._targetThread == thread)
            return &message;
    }
    return nullptr;
}

void OtherThreadsMessageDispatcher::routeReceivedMessage (std::unique_ptr<const IncomingMessage>&& message)
{
    ARA_INTERNAL_ASSERT (message);
    MessageID messageID;
    ThreadRef targetThread;
    [[maybe_unused]] auto success { _readFromMessage (message.get (), messageID) };
    success &= _readFromMessage (message.get (), targetThread);
    ARA_INTERNAL_ASSERT (success);

    if ((targetThread == getCurrentThread ()) ||    // message directed to this thread...
        (targetThread == _invalidThread))           // ...or no explicit thread specified (new transaction)
    {
        if (isReply (messageID))
        {
            ARA_INTERNAL_ASSERT (targetThread != _invalidThread);   // replies always must specify target thread
            ARA_IPC_LOG ("processes reply on" ARA_IPC_LABEL_THREAD_FORMAT, ARA_IPC_LABEL_THREAD_ARGS);
            ARA_INTERNAL_ASSERT (_pendingReplyHandler != nullptr);
            _handleReply (std::move (message), std::move (*_pendingReplyHandler->_replyHandler));
            _pendingReplyHandler = _pendingReplyHandler->_prevPendingReplyHandler;
        }
        else
        {
            ARA_IPC_LOG ("processes" ARA_IPC_DECODE_MESSAGE_FORMAT "on" ARA_IPC_LABEL_THREAD_FORMAT,
                         ARA_IPC_DECODE_RECEIVED_MESSAGE_ARGS (messageID), ARA_IPC_LABEL_THREAD_ARGS);
            _processReceivedMessage (messageID, std::move (message));
        }
    }
    else
    {
        if (isReply (messageID))
            ARA_IPC_LOG ("dispatches reply from" ARA_IPC_LABEL_THREAD_FORMAT " to sending thread %p", ARA_IPC_LABEL_THREAD_ARGS, targetThread);
        else
            ARA_IPC_LOG ("dispatches" ARA_IPC_DECODE_MESSAGE_FORMAT "from" ARA_IPC_LABEL_THREAD_FORMAT " to sending thread %p",
                         ARA_IPC_DECODE_RECEIVED_MESSAGE_ARGS (messageID), ARA_IPC_LABEL_THREAD_ARGS, targetThread);

        _routeLock.lock ();
        auto routedMessage { _getRoutedMessageForThread (_invalidThread) };
        if (message == nullptr)
            routedMessage = &_routedMessages.emplace_back (RoutedMessage {});
        routedMessage->_messageID = messageID;
        routedMessage->_message = std::move (message);
        routedMessage->_targetThread = targetThread;
        _routeReceiveCondition.notify_all ();
        _routeLock.unlock ();
    }
}

void OtherThreadsMessageDispatcher::_processReceivedMessage (MessageID messageID, std::unique_ptr<const IncomingMessage>&& message)
{
    const auto previousRemoteTargetThread { _remoteTargetThread };

    ThreadRef remoteTargetThread;
    [[maybe_unused]] const auto success { _readFromMessage (message.get (), remoteTargetThread) };
    ARA_INTERNAL_ASSERT (success && (remoteTargetThread != _invalidThread));
    _remoteTargetThread = remoteTargetThread;

    auto reply { getConnection ()->_createOutgoingMessage () };
    initializeMessage (reply.get (), 0);
    _handleReceivedMessage (messageID, std::move (message), reply.get ());

    _sendLock.lock ();
    _sendMessage (std::move (reply), 0);
    _sendLock.unlock ();

    _remoteTargetThread = previousRemoteTargetThread;
}

//------------------------------------------------------------------------------

Connection::Connection (OutgoingMessageFactory&& outgoingMessageFactory, MessageHandler&& messageHandler,
                        CreationThreadDispatcher&& creationThreadDispatcher,
                        bool receiverEndiannessMatches)
: _outgoingMessageFactory { std::move (outgoingMessageFactory) },
  _messageHandler { std::move (messageHandler) },
  _creationThreadDispatcher { std::move (creationThreadDispatcher) },
  _receiverEndiannessMatches { receiverEndiannessMatches },
  _creationThreadID { std::this_thread::get_id () }
{}

Connection::~Connection ()
{}

void Connection::setMainThreadChannel (std::unique_ptr<MessageChannel>&& messageChannel)
{
    ARA_INTERNAL_ASSERT (_mainThreadDispatcher == nullptr);
    _mainThreadDispatcher = std::make_unique<MainThreadMessageDispatcher> (this, std::move (messageChannel));
}

void Connection::setOtherThreadsChannel (std::unique_ptr<MessageChannel>&& messageChannel)
{
    ARA_INTERNAL_ASSERT (_otherThreadsDispatcher == nullptr);
    _otherThreadsDispatcher = std::make_unique<OtherThreadsMessageDispatcher> (this, std::move (messageChannel));
}

std::unique_ptr<OutgoingMessage> Connection::_createOutgoingMessage ()
{
    return _outgoingMessageFactory ();
}

void Connection::sendMessage (MessageID messageID, ArgumentsProvider&& argumentsProvider, ReplyHandler&& replyHandler)
{
    ARA_INTERNAL_ASSERT ((_mainThreadDispatcher != nullptr) && (_otherThreadsDispatcher != nullptr) && (_messageHandler != nullptr));

    const auto useMainThreadDispatcher { wasCreatedOnCurrentThread () };

    auto message { _createOutgoingMessage () };
    if (useMainThreadDispatcher)
        _mainThreadDispatcher->initializeMessage (message.get (), messageID);
    else
        _otherThreadsDispatcher->initializeMessage (message.get (), messageID);

    argumentsProvider (message.get ());

    if (useMainThreadDispatcher)
        _mainThreadDispatcher->sendMessage (std::move (message), std::move (replyHandler), messageID);
    else
        _otherThreadsDispatcher->sendMessage (std::move (message), std::move (replyHandler), messageID);
}

void Connection::processPendingMessageOnCreationThreadIfNeeded (ARATimeDuration timeout)
{
    _mainThreadDispatcher->processPendingMessageIfNeeded (timeout);
}

void Connection::_setDebugMessageHint (bool isHost)
{
    _debugAsHost = isHost;
}


}   // namespace IPC
}   // namespace ARA

#endif // ARA_ENABLE_IPC
