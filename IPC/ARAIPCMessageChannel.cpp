//------------------------------------------------------------------------------
//! \file       ARAIPCMessageChannel.cpp
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

#include "ARAIPCMessageChannel.h"


#if ARA_ENABLE_IPC


#include "ARA_Library/Debug/ARADebug.h"
#include "ARA_Library/IPC/ARAIPCEncoding.h"

#include <utility>

#if defined (__APPLE__)
    #include <CoreFoundation/CoreFoundation.h>
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
// For the main thread channel, this is trivial.
//
// For the channel that handles all communication on other threads, the implementation
// adds a token to each call message that identifies the sending thread. The reply message
// or any stacked call the receiver might make in response to the message will pass back
// this thread token, enabling proper routing back to the original sending thread.
// Since the actual ARA code is agnostic to IPC being used, the receiving side uses
// thread local storage to make the sender's thread token available for all stacked calls
// that the ARA code might make in response to the message.
//
// If a message is received that does not contain the token, this message indicates
// the start of a transaction that was initiated on the other side. For this new
// transaction, a proper thread has to be selected. This is done based on the message ID -
// the current implementation dispatches audio reading to a dedicated dispatch queue and
// handles the remaining calls directly on the IPC receive thread.
//
// Note that there is a crucial difference between the dispatch of a new transaction and
// the dispatch of any follow-up messages in the transaction: the initial message is dispatched
// to a thread that is potentially executing other code as well in some form of run loop,
// whereas the follow-ups need to dispatch to a thread that is currently blocking inside ARA code.


#if 0
    #define ARA_IPC_LOG(...) ARA_LOG ("ARA IPC " __VA_ARGS__)
#else
    #define ARA_IPC_LOG(...) ((void) 0)
#endif


namespace ARA {
namespace IPC {


// keys to store the threading information in the IPC messages
constexpr MessageArgumentKey sendThreadKey { -1 };
constexpr MessageArgumentKey receiveThreadKey { -2 };


#if defined (_WIN32)
    #define readThreadRef readInt32
    #define appendThreadRef appendInt32
#else
    #define readThreadRef readSize
    #define appendThreadRef appendSize
#endif


// actually a "static" member of ARAIPCChannel, but for some reason C++ doesn't allow this...
thread_local MessageDispatcher::ThreadRef _remoteTargetThread { 0 };


struct PendingReplyHandler
{
    Connection::ReplyHandler _replyHandler;
    void* _replyHandlerUserData;
};
// actually a "static" member of MessageChannel, but for some reason C++ doesn't allow this...
thread_local const PendingReplyHandler* _pendingReplyHandler { nullptr };


constexpr MessageDispatcher::ThreadRef MessageDispatcher::_invalidThread;


MessageDispatcher::MessageDispatcher (MessageChannel* messageChannel, MessageHandler* messageHandler, bool singleThreaded)
: _messageChannel { messageChannel },
  _messageHandler { messageHandler }
{
    static_assert (sizeof (std::thread::id) == sizeof (ThreadRef), "the current implementation relies on a specific thread ID size");
// unfortunately at least in clang std::thread::id's c'tor isn't constexpr
//  static_assert (std::thread::id {} == *reinterpret_cast<const std::thread::id*>(&_invalidThread), "the current implementation relies on invalid thread IDs being 0");
    ARA_INTERNAL_ASSERT (std::thread::id {} == *reinterpret_cast<const std::thread::id*>(&_invalidThread));
    
    _routedMessages.resize (12);     // we shouldn't use more than a handful of threads concurrently for the IPC

    if (!singleThreaded)
        _sendLock = new std::mutex;

    _messageChannel->setMessageDispatcher (this);
}

MessageDispatcher::~MessageDispatcher ()
{
    delete _sendLock;
    delete _messageChannel;
}

MessageDispatcher::ThreadRef MessageDispatcher::_getCurrentThread ()
{
    const auto thisThread { std::this_thread::get_id () };
    const auto result { *reinterpret_cast<const ThreadRef*> (&thisThread) };
    ARA_INTERNAL_ASSERT (result != _invalidThread);
    return result;
}

void MessageDispatcher::sendMessage (MessageID messageID, MessageEncoder* encoder,
                                     Connection::ReplyHandler replyHandler, void* replyHandlerUserData)
{
    const auto currentThread { _getCurrentThread () };
    encoder->appendThreadRef (sendThreadKey, currentThread);
    if (_remoteTargetThread != _invalidThread)
        encoder->appendThreadRef (receiveThreadKey, _remoteTargetThread);

    if (_sendLock)
        _sendLock->lock ();
    ARA_IPC_LOG ("sends message with ID %i on thread %p%s", messageID, currentThread, (_remoteTargetThread == _invalidThread) ? " (starting new transaction)" : "");
    _messageChannel->sendMessage (messageID, encoder);
    if (_sendLock)
        _sendLock->unlock ();

    if (_messageChannel->runsReceiveLoopOnCurrentThread ())
    {
        const PendingReplyHandler* previousPendingReplyHandler { _pendingReplyHandler };
        PendingReplyHandler pendingReplyHandler { replyHandler, replyHandlerUserData };
        _pendingReplyHandler = &pendingReplyHandler;
        do
        {
            _messageChannel->loopUntilMessageReceived ();
        } while (_pendingReplyHandler != nullptr);
        _pendingReplyHandler = previousPendingReplyHandler;
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

            if (receivedMessageID != 0)
            {
                _handleReceivedMessage (receivedMessageID, receivedDecoder);        // will also delete receivedDecoder
            }
            else
            {
                _handleReply (receivedDecoder, replyHandler, replyHandlerUserData); // will also delete receivedDecoder
                break;
            }
        }
    }

    delete encoder;
}

#if defined (_WIN32)
struct APCProcessReceivedMessageParams
{
    MessageDispatcher* dispatcher;
    MessageID messageID;
    const MessageDecoder* decoder;
};

void APCRouteNewTransactionFunc (ULONG_PTR parameter)
{
    auto params { reinterpret_cast<APCProcessReceivedMessageParams*> (parameter) };
    params->dispatcher->_handleReceivedMessage (params->messageID, params->decoder);
    delete params;
}
#endif

MessageDispatcher::RoutedMessage* MessageDispatcher::_getRoutedMessageForThread (ThreadRef thread)
{
    for (auto& message : _routedMessages)
    {
        if (message._targetThread == thread)
            return &message;
    }
    return nullptr;
}

void MessageDispatcher::routeReceivedMessage (MessageID messageID, const MessageDecoder* decoder)
{
    ThreadRef targetThread;
    if (decoder->readThreadRef (receiveThreadKey, &targetThread))
    {
        ARA_INTERNAL_ASSERT (targetThread != _invalidThread);
        if (targetThread == _getCurrentThread ())
        {
            ARA_INTERNAL_ASSERT (_messageChannel->runsReceiveLoopOnCurrentThread ());
            if (messageID != 0)
            {
                _handleReceivedMessage (messageID, decoder);
            }
            else
            {
                ARA_INTERNAL_ASSERT (_pendingReplyHandler != nullptr);
                _handleReply (decoder, _pendingReplyHandler->_replyHandler, _pendingReplyHandler->_replyHandlerUserData);
                _pendingReplyHandler = nullptr;
            }
        }
        else
        {
            if (messageID != 0)
                ARA_IPC_LOG ("dispatches received message with ID %i from thread %p to sending thread %p", messageID, _getCurrentThread(), targetThread);
            else
                ARA_IPC_LOG ("dispatches received reply from thread %p to sending thread %p", _getCurrentThread(), targetThread);

            _routeLock.lock ();
            RoutedMessage* message = _getRoutedMessageForThread (_invalidThread);
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
        ARA_INTERNAL_ASSERT (messageID != 0);
        if (const auto dispatchTarget { _messageHandler->getDispatchTargetForIncomingTransaction (messageID) })
        {
            ARA_IPC_LOG ("dispatches received message with ID %i (new transaction)", messageID);
#if defined (_WIN32)
            auto params { new APCProcessReceivedMessageParams { this, messageID, decoder } };
            const auto result { ::QueueUserAPC (APCRouteNewTransactionFunc, dispatchTarget, reinterpret_cast<ULONG_PTR> (params)) };
            ARA_INTERNAL_ASSERT (result != 0);
#elif defined (__APPLE__)
            dispatch_async (dispatchTarget,
                ^{
                    _handleReceivedMessage (messageID, decoder);
                });
#endif
        }
        else
        {
            _handleReceivedMessage (messageID, decoder);
        }
    }
}

void MessageDispatcher::_handleReceivedMessage (MessageID messageID, const MessageDecoder* decoder)
{
    ARA_IPC_LOG ("handles received message with ID %i on thread %p", messageID, _getCurrentThread());
    ARA_INTERNAL_ASSERT (messageID != 0);

    const auto previousRemoteTargetThread { _remoteTargetThread };
    ThreadRef remoteTargetThread;
    const auto success { decoder->readThreadRef (sendThreadKey, &remoteTargetThread) };
    ARA_INTERNAL_ASSERT (success);
    _remoteTargetThread = remoteTargetThread;

    auto replyEncoder { _messageHandler->handleReceivedMessage (messageID, decoder) };
    delete decoder;

    if (remoteTargetThread != _invalidThread)
        replyEncoder->appendThreadRef (receiveThreadKey, remoteTargetThread);

    if (_sendLock)
        _sendLock->lock ();
    ARA_IPC_LOG ("replies to message with ID %i on thread %p", messageID, _getCurrentThread());
    _messageChannel->sendMessage (0, replyEncoder);
    if (_sendLock)
        _sendLock->unlock ();

    delete replyEncoder;

    _remoteTargetThread = previousRemoteTargetThread;
}

void MessageDispatcher::_handleReply (const MessageDecoder* decoder, Connection::ReplyHandler replyHandler, void* replyHandlerUserData)
{
    ARA_IPC_LOG ("handles received reply on thread %p", _getCurrentThread());
    if (replyHandler)
        (replyHandler) (decoder, replyHandlerUserData);
    else
        ARA_INTERNAL_ASSERT (!decoder->hasDataForKey(0));   // replies should be empty when not handled (i.e. void)
    delete decoder;
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
    auto success { ConvertToRealHandle (::GetCurrentThread (), FALSE, &currentThread) };
    ARA_INTERNAL_ASSERT (success);
    return currentThread;
}
#endif


MessageHandler::MessageHandler ()
: _creationThreadID { std::this_thread::get_id () },
#if defined (_WIN32)
  _creationThreadDispatchTarget { _GetRealCurrentThread () }
#elif defined (__APPLE__)
  _creationThreadDispatchTarget { dispatch_get_main_queue () }
#else
    #error "not yet implemented on this platform"
#endif
{
#if defined (__APPLE__)
    // since there is no way to create a dispatch queue associated with the current thread,
    // we require this is called on the main thread on Apple platforms, which has the only
    // well-defined dispatch queue on the system.
    ARA_INTERNAL_ASSERT (CFRunLoopGetMain () == CFRunLoopGetCurrent ());
#endif
}

MessageHandler::DispatchTarget MessageHandler::getDispatchTargetForIncomingTransaction (MessageID /*messageID*/)
{
    return (std::this_thread::get_id () == _creationThreadID) ? nullptr : _creationThreadDispatchTarget;
}


Connection::Connection ()
: _creationThreadID { std::this_thread::get_id () }
{}

Connection::~Connection ()
{
    delete _otherDispatcher;
    delete _mainDispatcher;
}

void Connection::setupMainThreadChannel (MessageChannel* messageChannel, MessageHandler* messageHandler)
{
    ARA_INTERNAL_ASSERT (_mainDispatcher == nullptr);
    _mainDispatcher = new MessageDispatcher { messageChannel, messageHandler, true };
}

void Connection::setupOtherThreadsChannel (MessageChannel* messageChannel, MessageHandler* messageHandler)
{
    ARA_INTERNAL_ASSERT (_otherDispatcher == nullptr);
    _otherDispatcher = new MessageDispatcher { messageChannel, messageHandler, false };
}

void Connection::sendMessage (MessageID messageID, MessageEncoder* encoder, ReplyHandler replyHandler, void* replyHandlerUserData)
{
    if (std::this_thread::get_id () == _creationThreadID)
        _mainDispatcher->sendMessage (messageID, encoder, replyHandler, replyHandlerUserData);
    else
        _otherDispatcher->sendMessage (messageID, encoder, replyHandler, replyHandlerUserData);
}

}   // namespace IPC
}   // namespace ARA

#endif // ARA_ENABLE_IPC
