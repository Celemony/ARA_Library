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
    #define ARA_IPC_DECODE_SENT_MESSAGE_ARGS(messageID) messageID, (getConnection ()->sendsHostMessages ()) ? decodeHostMessageID (messageID) : decodePlugInMessageID (messageID)
    #define ARA_IPC_DECODE_RECEIVED_MESSAGE_ARGS(messageID) messageID, (!getConnection ()->sendsHostMessages ()) ? decodeHostMessageID (messageID) : decodePlugInMessageID (messageID)
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
// the incoming IPC messages on some undefined thread. In such cases, a dispatch
// from the receiving thread to the target thread is necessary, implemented via
// a condition variable that the receive thread awakes when a message comes in.
// Instead of looping, a sending thread will then wait on this condition for the reply.
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
// executed directly on the receive thread.
//
// Replies or callbacks are routed back to the originating thread in the sender. This is done
// by adding a token when sending a message that identifies the sending thread, and replies and
// callbacks pass this token back to allow for proper dispatching from the receive thread to the
// thread that initiated the transaction.
// Since the actual ARA code is agnostic to IPC being used, the receiving side uses
// thread local storage to make the sender's thread token available for all stacked calls
// that the ARA code might make in response to the message.
//
// Note that there is a crucial difference between the dispatch of a new transaction and
// the dispatch of any follow-up messages in the transaction: the initial message is dispatched
// to a thread that is potentially executing other code as well in some form of run loop,
// whereas the follow-ups need to dispatch to a thread that is currently blocking inside ARA code.


namespace ARA {
namespace IPC {


// key to indicate whether an outgoing call is made in response to a currently handled incoming call
// or a new call, which is necessary to deal with the decoupled main threads concurrency
// to optimize for performance, it is only added when the call is a response, being a new call is implicit
// (it is also never added to replies because they always are a response anyways)
constexpr MessageArgumentKey isResponseKey { -1 };

// keys to store the threading information in the IPC messages for OtherThreadsMessageDispatcher
constexpr MessageArgumentKey sendThreadKey { -1 };
constexpr MessageArgumentKey receiveThreadKey { -2 };


#if defined (_WIN32)
    #define readThreadRef readInt32
    #define appendThreadRef appendInt32
#else
    #define readThreadRef readSize
    #define appendThreadRef appendSize
#endif


MessageDispatcher::MessageDispatcher (Connection* connection, MessageChannel* messageChannel)
: _connection { connection },
  _messageChannel { messageChannel }
{
    static_assert (sizeof (std::thread::id) == sizeof (ThreadRef), "the current implementation relies on a specific thread ID size");
// unfortunately at least in clang std::thread::id's c'tor isn't constexpr
//  static_assert (std::thread::id {} == *reinterpret_cast<const std::thread::id*>(&_invalidThread), "the current implementation relies on invalid thread IDs being 0");
    ARA_INTERNAL_ASSERT (std::thread::id {} == *reinterpret_cast<const std::thread::id*>(&_invalidThread));
    
    _messageChannel->setMessageDispatcher (this);
}

MessageDispatcher::~MessageDispatcher ()
{
    delete _messageChannel;
}

MessageDispatcher::ThreadRef MessageDispatcher::getCurrentThread ()
{
    const auto thisThread { std::this_thread::get_id () };
    const auto result { *reinterpret_cast<const ThreadRef*> (&thisThread) };
    ARA_INTERNAL_ASSERT (result != _invalidThread);
    return result;
}

void MessageDispatcher::_sendMessage (MessageID messageID, MessageEncoder* encoder, [[maybe_unused]] bool isNewTransaction)
{
    if (isReply (messageID))
        ARA_IPC_LOG ("replies to message on" ARA_IPC_LABEL_THREAD_FORMAT, ARA_IPC_LABEL_THREAD_ARGS);
    else
        ARA_IPC_LOG ("sends" ARA_IPC_DECODE_MESSAGE_FORMAT "on" ARA_IPC_LABEL_THREAD_FORMAT "%s",
                     ARA_IPC_DECODE_SENT_MESSAGE_ARGS (messageID),  ARA_IPC_LABEL_THREAD_ARGS,
                     (isNewTransaction) ? " (new transaction)" : "");

    _messageChannel->sendMessage (messageID, encoder);

    delete encoder;
}

void MessageDispatcher::_handleReply (const MessageDecoder* decoder, Connection::ReplyHandler replyHandler, void* replyHandlerUserData)
{
    ARA_IPC_LOG ("handles reply on" ARA_IPC_LABEL_THREAD_FORMAT, ARA_IPC_LABEL_THREAD_ARGS);
    if (replyHandler)
        (replyHandler) (decoder, replyHandlerUserData);
    else
        ARA_INTERNAL_ASSERT (!decoder || !decoder->hasDataForKey (0));  // replies should be empty when not handled (i.e. void functions)
    delete decoder;
}

MessageEncoder* MessageDispatcher::_handleReceivedMessage (MessageID messageID, const MessageDecoder* decoder)
{
    ARA_INTERNAL_ASSERT (!isReply (messageID));

    ARA_IPC_LOG ("handles" ARA_IPC_DECODE_MESSAGE_FORMAT "on" ARA_IPC_LABEL_THREAD_FORMAT,
                 ARA_IPC_DECODE_RECEIVED_MESSAGE_ARGS (messageID), ARA_IPC_LABEL_THREAD_ARGS);
    auto replyEncoder { _connection->createEncoder () };
    _connection->getMessageHandler ()->handleReceivedMessage (messageID, decoder, replyEncoder);

    delete decoder;

    return replyEncoder;
}


void MainThreadMessageDispatcher::sendMessage (MessageID messageID, MessageEncoder* encoder,
                                               Connection::ReplyHandler replyHandler, void* replyHandlerUserData)
{
    ARA_INTERNAL_ASSERT (getConnection ()->wasCreatedOnCurrentThread ());
    ARA_INTERNAL_ASSERT (!isReply (messageID));

    const auto previousPendingReplyHandler { _pendingReplyHandler };
    const PendingReplyHandler pendingReplyHandler { replyHandler, replyHandlerUserData, previousPendingReplyHandler };
    _pendingReplyHandler = &pendingReplyHandler;

    const auto isResponse { _processingMessagesCount > 0 };
    if (isResponse)
        encoder->appendInt32 (isResponseKey, 1);

    _sendMessage (messageID, encoder, !isResponse);

    while (previousPendingReplyHandler != _pendingReplyHandler)
    {
        if (getConnection ()->waitForMessageOnCreationThread ())
            processPendingMessageIfNeeded ();
    }
}

void MainThreadMessageDispatcher::processPendingMessageIfNeeded ()
{
    ARA_INTERNAL_ASSERT (getConnection ()->wasCreatedOnCurrentThread ());

    const auto pendingMessageDecoder { _pendingMessageDecoder.exchange (reinterpret_cast<const MessageDecoder*> (_noPendingMessageDecoder), std::memory_order_acquire) };
    if (pendingMessageDecoder != reinterpret_cast<const MessageDecoder*> (_noPendingMessageDecoder))
    {
        const auto pendingMessageID { _pendingMessageID };
        if (isReply (pendingMessageID))
        {
            ARA_INTERNAL_ASSERT (_pendingReplyHandler != nullptr);
            _handleReply (pendingMessageDecoder, _pendingReplyHandler->_replyHandler, _pendingReplyHandler->_replyHandlerUserData);
            _pendingReplyHandler = _pendingReplyHandler->_prevPendingReplyHandler;
        }
        else
        {
            ++_processingMessagesCount;
            auto replyEncoder { _handleReceivedMessage (pendingMessageID, pendingMessageDecoder) };
            --_processingMessagesCount;
            _sendMessage (0, replyEncoder, false);
        }
    }
}

void MainThreadMessageDispatcher::routeReceivedMessage (MessageID messageID, const MessageDecoder* decoder)
{
    const auto isResponse { isReply (messageID) ||      // replies implicitly are responses
                            (decoder && decoder->hasDataForKey (isResponseKey)) };

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

    _pendingMessageID = messageID;
    _pendingMessageDecoder.store (decoder, std::memory_order_release);
    getConnection ()->signalMesssageReceived ();

    if (processSynchronously)
    {
        processPendingMessageIfNeeded ();
    }
    else
    {
        // only new transactions must be dispatched, otherwise the target thread is already waiting for the message received signal
        if (!isResponse)
            getConnection ()->dispatchToCreationThread (std::bind (&MainThreadMessageDispatcher::processPendingMessageIfNeeded, this));
    }
}


// actually "static" members of OtherThreadsMessageDispatcher, but for some reason C++ doesn't allow this...
thread_local OtherThreadsMessageDispatcher::ThreadRef _remoteTargetThread { 0 };
thread_local const OtherThreadsMessageDispatcher::PendingReplyHandler* _pendingReplyHandler { nullptr };

void OtherThreadsMessageDispatcher::sendMessage (MessageID messageID, MessageEncoder* encoder,
                                                 Connection::ReplyHandler replyHandler, void* replyHandlerUserData)
{
    const auto currentThread { getCurrentThread () };
    encoder->appendThreadRef (sendThreadKey, currentThread);
    const auto isResponse { _remoteTargetThread != _invalidThread };
    if (isResponse)
        encoder->appendThreadRef (receiveThreadKey, _remoteTargetThread);

    _sendLock.lock ();
    _sendMessage (messageID, encoder, !isResponse);
    _sendLock.unlock ();

    if (getConnection ()->wasCreatedOnCurrentThread ())
    {
        const auto previousPendingReplyHandler { _pendingReplyHandler };
        const PendingReplyHandler pendingReplyHandler { replyHandler, replyHandlerUserData, previousPendingReplyHandler };
        _pendingReplyHandler = &pendingReplyHandler;
        do
        {
            getConnection ()->waitForMessageOnCreationThread ();
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
            const auto receivedDecoder { message->_decoder };
            message->_targetThread = _invalidThread;
            lock.unlock ();

            if (isReply (receivedMessageID))
            {
                _handleReply (receivedDecoder, replyHandler, replyHandlerUserData); // will also delete receivedDecoder
                break;
            }
            else
            {
                _processReceivedMessage (receivedMessageID, receivedDecoder);       // will also delete receivedDecoder
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

void OtherThreadsMessageDispatcher::routeReceivedMessage (MessageID messageID, const MessageDecoder* decoder)
{
    ThreadRef targetThread;
    if (decoder->readThreadRef (receiveThreadKey, &targetThread))
    {
        ARA_INTERNAL_ASSERT (targetThread != _invalidThread);
        if (targetThread == getCurrentThread ())
        {
            if (isReply (messageID))
            {
                ARA_IPC_LOG ("processes reply on" ARA_IPC_LABEL_THREAD_FORMAT, ARA_IPC_LABEL_THREAD_ARGS);
                ARA_INTERNAL_ASSERT (_pendingReplyHandler != nullptr);
                _handleReply (decoder, _pendingReplyHandler->_replyHandler, _pendingReplyHandler->_replyHandlerUserData);
                _pendingReplyHandler = _pendingReplyHandler->_prevPendingReplyHandler;
            }
            else
            {
                ARA_IPC_LOG ("processes" ARA_IPC_DECODE_MESSAGE_FORMAT "on" ARA_IPC_LABEL_THREAD_FORMAT,
                             ARA_IPC_DECODE_RECEIVED_MESSAGE_ARGS (messageID), ARA_IPC_LABEL_THREAD_ARGS);
                _processReceivedMessage (messageID, decoder);
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
            message->_decoder = decoder;
            message->_targetThread = targetThread;
            _routeReceiveCondition.notify_all ();
            _routeLock.unlock ();
        }
    }
    else
    {
        ARA_INTERNAL_ASSERT (!isReply (messageID));
        _processReceivedMessage (messageID, decoder);
    }
}

void OtherThreadsMessageDispatcher::_processReceivedMessage (MessageID messageID, const MessageDecoder* decoder)
{
    const auto previousRemoteTargetThread { _remoteTargetThread };
    ThreadRef remoteTargetThread;
    [[maybe_unused]] const auto success { decoder->readThreadRef (sendThreadKey, &remoteTargetThread) };
    ARA_INTERNAL_ASSERT (success);
    _remoteTargetThread = remoteTargetThread;

    auto replyEncoder { _handleReceivedMessage (messageID, decoder) };

    replyEncoder->appendThreadRef (receiveThreadKey, remoteTargetThread);

    _sendLock.lock ();
    _sendMessage (0, replyEncoder, false);
    _sendLock.unlock ();

    _remoteTargetThread = previousRemoteTargetThread;
}


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

void Connection::performRunloopSource (void* info)
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


Connection::Connection (WaitForMessageDelegate waitForMessageDelegate, void* delegateUserData)
: _waitForMessageDelegate { waitForMessageDelegate },
  _delegateUserData { delegateUserData },
#if __cplusplus >= 202002L
  _waitForMessageSemaphore { new std::binary_semaphore { 0 } },
#elif defined (_WIN32)
  _waitForMessageSemaphore { ::CreateSemaphore (nullptr, 0, LONG_MAX, nullptr) },
#elif defined (__APPLE__)
  _waitForMessageSemaphore { dispatch_semaphore_create (0) },
#else
    #error "IPC not yet implemented for this platform"
#endif
  _creationThreadID { std::this_thread::get_id () },
#if defined (_WIN32)
  _creationThreadHandle { _GetRealCurrentThread () }
{}
#elif defined (__APPLE__)
  _creationThreadRunLoop { CFRunLoopGetCurrent () }
{
    CFRunLoopSourceContext context { 0, this, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, performRunloopSource };
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
    delete _otherThreadsDispatcher;
    delete _mainThreadDispatcher;
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

void Connection::setMainThreadChannel (MessageChannel* messageChannel)
{
    ARA_INTERNAL_ASSERT (_mainThreadDispatcher == nullptr);
    _mainThreadDispatcher = new MainThreadMessageDispatcher { this, messageChannel };
}

void Connection::setOtherThreadsChannel (MessageChannel* messageChannel)
{
    ARA_INTERNAL_ASSERT (_otherThreadsDispatcher == nullptr);
    _otherThreadsDispatcher = new OtherThreadsMessageDispatcher { this, messageChannel };
}

void Connection::setMessageHandler (MessageHandler* messageHandler)
{
    ARA_INTERNAL_ASSERT (_messageHandler == nullptr);
    _messageHandler = messageHandler;
}

void Connection::sendMessage (MessageID messageID, MessageEncoder* encoder, ReplyHandler replyHandler, void* replyHandlerUserData)
{
    ARA_INTERNAL_ASSERT ((_mainThreadDispatcher != nullptr) && (_otherThreadsDispatcher != nullptr) && (_messageHandler != nullptr));
    if (wasCreatedOnCurrentThread ())
        _mainThreadDispatcher->sendMessage (messageID, encoder, replyHandler, replyHandlerUserData);
    else
        _otherThreadsDispatcher->sendMessage (messageID, encoder, replyHandler, replyHandlerUserData);
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

bool Connection::waitForMessageOnCreationThread ()
{
    ARA_INTERNAL_ASSERT (wasCreatedOnCurrentThread ());

    constexpr ARATimeDuration timeout { 0.010 };
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
        return true;

    if (_waitForMessageDelegate)
        _waitForMessageDelegate (_delegateUserData);

    return false;
}

void Connection::signalMesssageReceived ()
{
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


}   // namespace IPC
}   // namespace ARA

#endif // ARA_ENABLE_IPC
