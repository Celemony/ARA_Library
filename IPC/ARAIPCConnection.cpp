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

#include <utility>
#include <optional>
#if __cplusplus >= 202002L
    #include <chrono>
    #include <semaphore>
#endif

#if defined (__APPLE__)
    #include <CoreFoundation/CoreFoundation.h>
#endif


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


#if defined (_WIN32)
    #define readThreadRef readInt32
    #define appendThreadRef appendInt32
#else
    #define readThreadRef readSize
    #define appendThreadRef appendSize
#endif

static bool _debugAsHost {};

//------------------------------------------------------------------------------

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
    explicit MessageDispatcher (Connection* connection, std::unique_ptr<MessageChannel> && messageChannel);
    virtual ~MessageDispatcher () = default;

// protected: this does not work with thread_local...
    struct PendingReplyHandler
    {
        ReplyHandler* const _replyHandler;
        const PendingReplyHandler* _prevPendingReplyHandler;
    };

    static bool isReply (MessageID messageID) { return messageID == 0; }

    static ThreadRef getCurrentThread ();

protected:
    Connection* getConnection () const { return  _connection; }
    MessageChannel* getMessageChannel () const { return  _messageChannel.get (); }

    void _sendMessage (MessageID messageID, std::unique_ptr<MessageEncoder> && encoder, bool isNewTransaction);
    bool _waitForMessage ();
    void _handleReply (std::unique_ptr<const MessageDecoder> && decoder, ReplyHandler && replyHandler);
    std::unique_ptr<MessageEncoder> _handleReceivedMessage (MessageID messageID, std::unique_ptr<const MessageDecoder> && decoder);

private:
    Connection* const _connection;
    const std::unique_ptr<MessageChannel> _messageChannel;
};


MessageDispatcher::MessageDispatcher (Connection* connection, std::unique_ptr<MessageChannel> && messageChannel)
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

void MessageDispatcher::_sendMessage (MessageID messageID, std::unique_ptr<MessageEncoder> && encoder, [[maybe_unused]] bool isNewTransaction)
{
    if (isReply (messageID))
        ARA_IPC_LOG ("replies to message on" ARA_IPC_LABEL_THREAD_FORMAT, ARA_IPC_LABEL_THREAD_ARGS);
    else
        ARA_IPC_LOG ("sends" ARA_IPC_DECODE_MESSAGE_FORMAT "on" ARA_IPC_LABEL_THREAD_FORMAT "%s",
                     ARA_IPC_DECODE_SENT_MESSAGE_ARGS (messageID),  ARA_IPC_LABEL_THREAD_ARGS,
                     (isNewTransaction) ? " (new transaction)" : "");

    _messageChannel->sendMessage (messageID, std::move (encoder));
}

void MessageDispatcher::_handleReply (std::unique_ptr<const MessageDecoder> && decoder, ReplyHandler && replyHandler)
{
    ARA_IPC_LOG ("handles reply on" ARA_IPC_LABEL_THREAD_FORMAT, ARA_IPC_LABEL_THREAD_ARGS);
    if (replyHandler)
        replyHandler (decoder.get ());
    else
        ARA_INTERNAL_ASSERT (!decoder || !decoder->hasDataForKey (0));  // replies should be empty when not handled (i.e. void functions)
}

std::unique_ptr<MessageEncoder> MessageDispatcher::_handleReceivedMessage (MessageID messageID, std::unique_ptr<const MessageDecoder> && decoder)
{
    ARA_INTERNAL_ASSERT (!isReply (messageID));

    ARA_IPC_LOG ("handles" ARA_IPC_DECODE_MESSAGE_FORMAT "on" ARA_IPC_LABEL_THREAD_FORMAT,
                 ARA_IPC_DECODE_RECEIVED_MESSAGE_ARGS (messageID), ARA_IPC_LABEL_THREAD_ARGS);
    auto replyEncoder { _connection->createEncoder () };
    _connection->getMessageHandler () (messageID, decoder.get (), replyEncoder.get ());

    return replyEncoder;
}

//------------------------------------------------------------------------------

// helper for MainThreadMessageDispatcher: single-object message queue with semaphore to wait on
class WaitableSingleMessageQueue
{
    public:
        WaitableSingleMessageQueue ()
#if __cplusplus >= 202002L
        : _waitForMessageSemaphore { new std::binary_semaphore { 0 } }
#elif defined (_WIN32)
        : _waitForMessageSemaphore { ::CreateSemaphore (nullptr, 0, LONG_MAX, nullptr) }
#elif defined (__APPLE__)
        : _waitForMessageSemaphore { dispatch_semaphore_create (0) }
#else
#error "IPC not yet implemented for this platform"
#endif
        {}

        ~WaitableSingleMessageQueue ()
        {
#if __cplusplus >= 202002L
            delete static_cast<std::binary_semaphore*> (_waitForMessageSemaphore);
#elif defined (_WIN32)
            ::CloseHandle (_waitForMessageSemaphore);
#elif defined (__APPLE__)
            dispatch_release (static_cast<dispatch_semaphore_t> (_waitForMessageSemaphore));
#else
#error "IPC not yet implemented for this platform"
#endif
        }

        std::optional<std::pair<MessageID, std::unique_ptr<const MessageDecoder>>> waitOnSemaphore (ARATimeDuration timeout)
        {
            bool didReceiveMessage;
#if __cplusplus >= 202002L
            didReceiveMessage = static_cast<std::binary_semaphore*> (_waitForMessageSemaphore)->try_acquire_for (std::chrono::duration<ARATimeDuration> { timeout });
#elif defined (_WIN32)
            didReceiveMessage = (::WaitForSingleObject (_waitForMessageSemaphore, static_cast<DWORD> (timeout * 1000.0 + 0.5)) == WAIT_OBJECT_0);
#elif defined (__APPLE__)
            const auto deadline { dispatch_time (DISPATCH_TIME_NOW, static_cast<int64_t> (10e9 * timeout + 0.5)) };
            didReceiveMessage = (dispatch_semaphore_wait (static_cast<dispatch_semaphore_t> (_waitForMessageSemaphore), deadline) == 0);
#else
#error "IPC not yet implemented for this platform"
#endif
            if (didReceiveMessage)
            {
                const auto messageID { _pendingMessageID };
                const auto messageDecoder { _pendingMessageDecoder.load (std::memory_order_acquire) };
                return std::make_pair (messageID, std::unique_ptr<const MessageDecoder> (messageDecoder));
            }
            else
            {
                return {};
            }
        }

        void signalSemaphore (MessageID messageID, std::unique_ptr<const MessageDecoder> && decoder)
        {
            _pendingMessageID = messageID;
            _pendingMessageDecoder.store (decoder.release (), std::memory_order_release);
#if __cplusplus >= 202002L
            static_cast<std::binary_semaphore*> (_waitForMessageSemaphore)->release ();
#elif defined (_WIN32)
            ::ReleaseSemaphore (_waitForMessageSemaphore, 1, nullptr);
#elif defined (__APPLE__)
            dispatch_semaphore_signal (static_cast<dispatch_semaphore_t> (_waitForMessageSemaphore));
#else
#error "IPC not yet implemented for this platform"
#endif
        }

    private:
        MessageID _pendingMessageID { 0 };      // read/write _pendingMessageDecoder with proper barrier before/after reading/writing this
        std::atomic<const MessageDecoder*> _pendingMessageDecoder { nullptr };
        void* const _waitForMessageSemaphore;   // concrete type is platform-dependent
};

//------------------------------------------------------------------------------

// single-threaded variant for main thread communication only
class MainThreadMessageDispatcher : public MessageDispatcher
{
public:
    MainThreadMessageDispatcher (Connection* connection, std::unique_ptr<MessageChannel> && messageChannel)
    : MessageDispatcher { connection, std::move (messageChannel) }
    {
        getMessageChannel ()->_receivedMessageRouter = [this] (MessageID messageID, std::unique_ptr<const MessageDecoder> && decoder)
                                                              { routeReceivedMessage (messageID, std::move (decoder)); };
    }

    ~MainThreadMessageDispatcher () override = default;

    void sendMessage (MessageID messageID, std::unique_ptr<MessageEncoder> && encoder, ReplyHandler && replyHandler);

    void routeReceivedMessage (MessageID messageID, std::unique_ptr<const MessageDecoder> && decoder);

    void processPendingMessageIfNeeded ()
    {
        ARA_INTERNAL_ASSERT (getConnection ()->wasCreatedOnCurrentThread ());

        auto receivedData { _messageQueue.waitOnSemaphore (0.0) };
        if (receivedData)
            processMessage (receivedData->first, std::move (receivedData->second));
    }

protected:
    void processMessage (MessageID messageID, std::unique_ptr<const MessageDecoder> && decoder);

private:
    // key to indicate whether an outgoing call is made in response (reply or callback) to a
    // currently handled incoming call, or a new call
    // This distinction is necessary to deal with the decoupled main threads concurrency.
    // To optimize for performance, it is only added when the call is a response, while being a
    // new call is implicit. It is also not added to replies because they always are a response.
    static constexpr MessageArgumentKey kIsResponseKey { -1 };

private:
    int32_t _processingMessagesCount { 0 };

    const PendingReplyHandler* _pendingReplyHandler { nullptr };

    WaitableSingleMessageQueue _messageQueue {};
};


void MainThreadMessageDispatcher::sendMessage (MessageID messageID, std::unique_ptr<MessageEncoder> && encoder,
                                               ReplyHandler && replyHandler)
{
    ARA_INTERNAL_ASSERT (getConnection ()->wasCreatedOnCurrentThread ());
    ARA_INTERNAL_ASSERT (!isReply (messageID));

    const auto previousPendingReplyHandler { _pendingReplyHandler };
    const PendingReplyHandler pendingReplyHandler { &replyHandler, previousPendingReplyHandler };
    _pendingReplyHandler = &pendingReplyHandler;

    const auto isResponse { _processingMessagesCount > 0 };
    if (isResponse)
        encoder->appendInt32 (kIsResponseKey, 1);

    _sendMessage (messageID, std::move (encoder), !isResponse);

    while (previousPendingReplyHandler != _pendingReplyHandler)
    {
        if (getMessageChannel ()->receivesMessagesOnCurrentThread ())
        {
            if (!getMessageChannel ()->waitForMessageOnCurrentThread ())
                getConnection ()->_callWaitForMessageDelegate ();
        }
        else
        {
            auto receivedData { _messageQueue.waitOnSemaphore (0.010) };
            if (receivedData )
                processMessage (receivedData->first, std::move (receivedData->second));
            else
                getConnection ()->_callWaitForMessageDelegate ();
        }
    }
}

void MainThreadMessageDispatcher::processMessage (MessageID messageID, std::unique_ptr<const MessageDecoder> && decoder)
{
    ARA_INTERNAL_ASSERT (getConnection ()->wasCreatedOnCurrentThread ());

    if (isReply (messageID))
    {
        ARA_INTERNAL_ASSERT (_pendingReplyHandler != nullptr);
        _handleReply (std::move (decoder), std::move (*_pendingReplyHandler->_replyHandler));
        _pendingReplyHandler = _pendingReplyHandler->_prevPendingReplyHandler;
    }
    else
    {
        ++_processingMessagesCount;
        auto replyEncoder { _handleReceivedMessage (messageID, std::move (decoder)) };
        --_processingMessagesCount;
        _sendMessage (0, std::move (replyEncoder), false);
    }
}

void MainThreadMessageDispatcher::routeReceivedMessage (MessageID messageID, std::unique_ptr<const MessageDecoder> && decoder)
{
    const auto isResponse { isReply (messageID) ||      // replies implicitly are responses
                            (decoder && decoder->hasDataForKey (kIsResponseKey)) };

    // if on creation thread, responses can be processed immediately, and
    // new transaction can only be processed immediately when no other transaction is going on
    const auto processSynchronously { getConnection ()->wasCreatedOnCurrentThread () &&
                                      (isResponse || (_pendingReplyHandler == nullptr)) };

    if (processSynchronously)
    {
        if (isReply (messageID))
            ARA_IPC_LOG ("processes reply on" ARA_IPC_LABEL_THREAD_FORMAT, ARA_IPC_LABEL_THREAD_ARGS);
        else
            ARA_IPC_LOG ("processes" ARA_IPC_DECODE_MESSAGE_FORMAT "on" ARA_IPC_LABEL_THREAD_FORMAT,
                         ARA_IPC_DECODE_RECEIVED_MESSAGE_ARGS (messageID), ARA_IPC_LABEL_THREAD_ARGS);
    }
    else
    {
        if (isReply (messageID))
            ARA_IPC_LOG ("dispatches reply from" ARA_IPC_LABEL_THREAD_FORMAT " to creation thread", ARA_IPC_LABEL_THREAD_ARGS);
        else
            ARA_IPC_LOG ("dispatches" ARA_IPC_DECODE_MESSAGE_FORMAT "from" ARA_IPC_LABEL_THREAD_FORMAT " to creation thread",
                         ARA_IPC_DECODE_RECEIVED_MESSAGE_ARGS (messageID), ARA_IPC_LABEL_THREAD_ARGS);
    }

    if (processSynchronously)
    {
        processMessage (messageID, std::move (decoder));
    }
    else
    {
        // only new transactions must be dispatched, otherwise the target thread is already waiting for the message received signal
        if (isResponse)
            _messageQueue.signalSemaphore (messageID, std::move (decoder));
        else
            getConnection ()->dispatchToCreationThread ([this, messageID, d = decoder.release ()] ()
                                                        { processMessage (messageID, std::unique_ptr<const MessageDecoder> (d)); });
    }
}

//------------------------------------------------------------------------------

// multi-threaded variant for all non-main thread communication
class OtherThreadsMessageDispatcher : public MessageDispatcher
{
public:
    OtherThreadsMessageDispatcher (Connection* connection, std::unique_ptr<MessageChannel> && messageChannel)
    : MessageDispatcher { connection, std::move (messageChannel) }
    {
        getMessageChannel ()->_receivedMessageRouter = [this] (MessageID messageID, std::unique_ptr<const MessageDecoder> && decoder)
                                                              { routeReceivedMessage (messageID, std::move (decoder)); };
    }

    void sendMessage (MessageID messageID, std::unique_ptr<MessageEncoder> && encoder, ReplyHandler && replyHandler);

    void routeReceivedMessage (MessageID messageID, std::unique_ptr<const MessageDecoder> && decoder);

private:
    // keys to store the threading information in the IPC messages
    static constexpr MessageArgumentKey kSendThreadKey { -1 };
    static constexpr MessageArgumentKey kReceiveThreadKey { -2 };

    struct RoutedMessage
    {
        MessageID _messageID { 0 };
        std::unique_ptr<const MessageDecoder> _decoder;
        ThreadRef _targetThread { _invalidThread };
    };
    RoutedMessage* _getRoutedMessageForThread (ThreadRef thread);

    void _processReceivedMessage (MessageID messageID, std::unique_ptr<const MessageDecoder> && decoder);

private:
    // incoming data is stored in _routedMessages by the receive handler for the
    // sending threads waiting to pick it up (signalled via _routeReceiveCondition)
    std::condition_variable _routeReceiveCondition;
    std::vector<RoutedMessage> _routedMessages { 12 };  // we shouldn't use more than a handful of threads concurrently for the IPC
    std::mutex _sendLock;
    std::mutex _routeLock;
};

// actually "static" members of OtherThreadsMessageDispatcher, but for some reason C++ doesn't allow this...
thread_local OtherThreadsMessageDispatcher::ThreadRef _remoteTargetThread { 0 };
thread_local const OtherThreadsMessageDispatcher::PendingReplyHandler* _pendingReplyHandler { nullptr };

void OtherThreadsMessageDispatcher::sendMessage (MessageID messageID, std::unique_ptr<MessageEncoder> && encoder,
                                                 ReplyHandler && replyHandler)
{
    ARA_INTERNAL_ASSERT (!getConnection ()->wasCreatedOnCurrentThread ());

    const auto currentThread { getCurrentThread () };
    encoder->appendThreadRef (kSendThreadKey, currentThread);
    const auto isResponse { _remoteTargetThread != _invalidThread };
    if (isResponse)
        encoder->appendThreadRef (kReceiveThreadKey, _remoteTargetThread);

    _sendLock.lock ();
    _sendMessage (messageID, std::move (encoder), !isResponse);
    _sendLock.unlock ();


    if (getMessageChannel ()->receivesMessagesOnCurrentThread ())
    {
        const auto previousPendingReplyHandler { _pendingReplyHandler };
        const PendingReplyHandler pendingReplyHandler { &replyHandler, previousPendingReplyHandler };
        _pendingReplyHandler = &pendingReplyHandler;
        do
        {
            if (!getMessageChannel ()->waitForMessageOnCurrentThread ())
                getConnection ()->_callWaitForMessageDelegate ();
        } while (_pendingReplyHandler != previousPendingReplyHandler);
    }
    else
    {
        while (true)
        {
            std::unique_lock <std::mutex> lock { _routeLock };
            _routeReceiveCondition.wait (lock, [this, &currentThread]
                                    { return _getRoutedMessageForThread (currentThread) != nullptr; });
            RoutedMessage* message = _getRoutedMessageForThread (currentThread);
            const auto receivedMessageID { message->_messageID };
            auto receivedDecoder { std::move (message->_decoder) };
            message->_targetThread = _invalidThread;
            lock.unlock ();

            if (isReply (receivedMessageID))
            {
                _handleReply (std::move (receivedDecoder), std::move (replyHandler));
                break;
            }
            else
            {
                _processReceivedMessage (receivedMessageID, std::move (receivedDecoder));
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

void OtherThreadsMessageDispatcher::routeReceivedMessage (MessageID messageID, std::unique_ptr<const MessageDecoder> && decoder)
{
    ThreadRef targetThread;
    if (decoder->readThreadRef (kReceiveThreadKey, &targetThread))
    {
        ARA_INTERNAL_ASSERT (targetThread != _invalidThread);
        if (targetThread == getCurrentThread ())
        {
            if (isReply (messageID))
            {
                ARA_IPC_LOG ("processes reply on" ARA_IPC_LABEL_THREAD_FORMAT, ARA_IPC_LABEL_THREAD_ARGS);
                ARA_INTERNAL_ASSERT (_pendingReplyHandler != nullptr);
                _handleReply (std::move (decoder), std::move (*_pendingReplyHandler->_replyHandler));
                _pendingReplyHandler = _pendingReplyHandler->_prevPendingReplyHandler;
            }
            else
            {
                ARA_IPC_LOG ("processes" ARA_IPC_DECODE_MESSAGE_FORMAT "on" ARA_IPC_LABEL_THREAD_FORMAT,
                             ARA_IPC_DECODE_RECEIVED_MESSAGE_ARGS (messageID), ARA_IPC_LABEL_THREAD_ARGS);
                _processReceivedMessage (messageID, std::move (decoder));
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
            RoutedMessage* message { _getRoutedMessageForThread (_invalidThread) };
            if (message == nullptr)
            {
                _routedMessages.push_back ({});
                message = &_routedMessages.back ();
            }
            message->_messageID = messageID;
            message->_decoder = std::move (decoder);
            message->_targetThread = targetThread;
            _routeReceiveCondition.notify_all ();
            _routeLock.unlock ();
        }
    }
    else
    {
        ARA_INTERNAL_ASSERT (!isReply (messageID));
        _processReceivedMessage (messageID, std::move (decoder));
    }
}

void OtherThreadsMessageDispatcher::_processReceivedMessage (MessageID messageID, std::unique_ptr<const MessageDecoder> && decoder)
{
    const auto previousRemoteTargetThread { _remoteTargetThread };
    ThreadRef remoteTargetThread;
    [[maybe_unused]] const auto success { decoder->readThreadRef (kSendThreadKey, &remoteTargetThread) };
    ARA_INTERNAL_ASSERT (success);
    ARA_INTERNAL_ASSERT (remoteTargetThread != _invalidThread);
    _remoteTargetThread = remoteTargetThread;

    auto replyEncoder { _handleReceivedMessage (messageID, std::move (decoder)) };

    replyEncoder->appendThreadRef (kReceiveThreadKey, remoteTargetThread);

    _sendLock.lock ();
    _sendMessage (0, std::move (replyEncoder), false);
    _sendLock.unlock ();

    _remoteTargetThread = previousRemoteTargetThread;
}

//------------------------------------------------------------------------------

#if defined (_WIN32)

// from https://devblogs.microsoft.com/oldnewthing/20141015-00/?p=43843
BOOL ConvertToRealHandle(HANDLE h,
                         BOOL bInheritHandle,
                         HANDLE *phConverted)
{
 return DuplicateHandle(GetCurrentProcess(), h,
                        GetCurrentProcess(), phConverted,
                        0, bInheritHandle, DUPLICATE_SAME_ACCESS);
}

HANDLE _GetRealCurrentThread ()
{
    HANDLE currentThread {};
    [[maybe_unused]] const auto success { ConvertToRealHandle (::GetCurrentThread (), FALSE, &currentThread) };
    ARA_INTERNAL_ASSERT (success);
    return currentThread;
}

void APCRouteNewTransactionFunc (ULONG_PTR parameter)
{
    auto funcPtr { reinterpret_cast<Connection::DispatchableFunction*> (parameter) };
    (*funcPtr) ();
    delete funcPtr;
}

#elif defined (__APPLE__)

void Connection::_performRunLoopSource (void* info)
{
    auto connection { reinterpret_cast<Connection*> (info) };
    connection->_mutex.lock ();
    while (!connection->_queue.empty ())
    {
        auto func { connection->_queue.front () };
        connection->_queue.pop ();
        connection->_mutex.unlock ();
 
        func ();
 
        connection->_mutex.lock ();
    }
    connection->_mutex.unlock ();
}

#endif


Connection::Connection (MessageEncoderFactory && messageEncoderFactory, MessageHandler&& messageHandler,
                        bool receiverEndianessMatches, WaitForMessageDelegate && waitForMessageDelegate)
: _messageEncoderFactory { std::move (messageEncoderFactory) },
  _messageHandler { std::move (messageHandler) },
  _receiverEndianessMatches { receiverEndianessMatches },
  _waitForMessageDelegate { std::move (waitForMessageDelegate) },
  _creationThreadID { std::this_thread::get_id () },
#if defined (_WIN32)
  _creationThreadHandle { _GetRealCurrentThread () }
{}
#elif defined (__APPLE__)
  _creationThreadRunLoop { CFRunLoopGetCurrent () }
{
    CFRunLoopSourceContext context { 0, this, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, _performRunLoopSource };
    _runloopSource = CFRunLoopSourceCreate (kCFAllocatorDefault, 0, &context);
    CFRunLoopAddSource (_creationThreadRunLoop, _runloopSource, kCFRunLoopCommonModes);
}
#endif

Connection::~Connection ()
{
#if defined (__APPLE__)
    CFRunLoopRemoveSource (_creationThreadRunLoop, _runloopSource, kCFRunLoopCommonModes);
    CFRelease (_runloopSource);
#endif
}

void Connection::setMainThreadChannel (std::unique_ptr<MessageChannel> && messageChannel)
{
    ARA_INTERNAL_ASSERT (_mainThreadDispatcher == nullptr);
    _mainThreadDispatcher = std::make_unique<MainThreadMessageDispatcher> (this, std::move (messageChannel));
}

void Connection::setOtherThreadsChannel (std::unique_ptr<MessageChannel> && messageChannel)
{
    ARA_INTERNAL_ASSERT (_otherThreadsDispatcher == nullptr);
    _otherThreadsDispatcher = std::make_unique<OtherThreadsMessageDispatcher> (this, std::move (messageChannel));
}

void Connection::sendMessage (MessageID messageID, std::unique_ptr<MessageEncoder> && encoder, ReplyHandler && replyHandler)
{
    ARA_INTERNAL_ASSERT ((_mainThreadDispatcher != nullptr) && (_otherThreadsDispatcher != nullptr) && (_messageHandler != nullptr));
    if (wasCreatedOnCurrentThread ())
        _mainThreadDispatcher->sendMessage (messageID, std::move (encoder), std::move (replyHandler));
    else
        _otherThreadsDispatcher->sendMessage (messageID, std::move (encoder), std::move (replyHandler));
}

void Connection::dispatchToCreationThread (DispatchableFunction func)
{
#if defined (_WIN32)
    auto funcPtr { new DispatchableFunction { func } };
    [[maybe_unused]] const auto result { ::QueueUserAPC (APCRouteNewTransactionFunc, _creationThreadHandle, reinterpret_cast<ULONG_PTR> (funcPtr)) };
    ARA_INTERNAL_ASSERT (result != 0);
#elif defined (__APPLE__)
    _mutex.lock ();
    _queue.emplace (std::move (func));
    _mutex.unlock ();
    CFRunLoopSourceSignal (_runloopSource);
    CFRunLoopWakeUp (_creationThreadRunLoop);
#endif
}

void Connection::processPendingMessageOnCreationThreadIfNeeded ()
{
    _mainThreadDispatcher->processPendingMessageIfNeeded ();
}

void Connection::_callWaitForMessageDelegate ()
{
    if (_waitForMessageDelegate)
        _waitForMessageDelegate ();
}

void Connection::_setDebugMessageHint (bool isHost)
{
    _debugAsHost = isHost;
}


}   // namespace IPC
}   // namespace ARA

#endif // ARA_ENABLE_IPC
