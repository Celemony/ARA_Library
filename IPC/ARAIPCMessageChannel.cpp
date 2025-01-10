//------------------------------------------------------------------------------
//! \file       MessageChannel.cpp
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
// From that method, the host now tests which content types are available and
// eventually call createAudioSourceContentReader() to read the content data.
// In the code below, such a stack of messages is referred to as a "transaction".
//
// However, many IPC APIs including Audio Unit message channels cannot be stacked,
// i.e. each message has to completely be processed before the next one can be sent.
// Therefore, each ARA API calls is split into two IPC messages:
// the actual call message that does not return any result just yet, and a matching
// reply message that returns the result (even if it is void, because the caller
// needs to know when the call has completed).
// After sending the actual call message, the sender loops while listening for
// incoming messages, which might either be a stacked callback that is processed
// accordingly, or the result reply which ends the loop.
//
// In this loop, extra efforts must be done to handle threading:
// When e.g. using Audio Unit message channels, Grand Central Dispatch will deliver
// the incoming IPC messages on some undefined thread, from which they need to be
// transferred to the sending thread for proper handling.
// In such cases, instead of looping the sending thread waits on a condition
// variable that the receive thread awakes when a message comes in.
//
// While most ARA communication is happening on the main thread, there are
// some calls that may be made from other threads. This poses several challenges
// when tunnelling everything through a single(threaded) IPC channel.
// First of all, there needs to be a lock around actually accessing the channel.
// Both hosts and plug-ins will need to carefully evaluate their existing ARA
// code to check for potential deadlocks or priority inversion caused by this.
//
// Another challenge when using IPC between host and plug-in is that for each
// message that comes in, an appropriate thread has to be selected to process it.
// The implementation therefore adds a thread token to each message to that is
// passed back along the reply or any stacked call so that these can be routed
// to the sending thread.
// However if an incoming call starts a new transaction, some appropriate thread
// has to be chosen on the receiving end. Fortunately, the current threading
// restrictions of ARA allow for a fairly simple pattern to address this:
// All transactions that the host initiates are processed on the plug-in's main
// thread because the vast majority of calls is restricted to the main thread
// anyways, and the few calls that may be made on other threads are all allowed
// on the main thread, too. A particular noteworthy example for this is
// getPlaybackRegionHeadAndTailTime(), which is allowed to be called on render
// threads: when the host decides to use XPC, it should not poll this at realtime
// but rather update it whenever there's a notifyPlaybackRegionContentChanged().
// All transactions started from the plug-in (readAudioSamples() and the
// functions in ARAPlaybackControllerInterface) are calls that can come from
// any thread and thus are directly processed on the IPC thread in the host.
// (Remember, the side that started a transaction will see all calls on the
// thread that started it.)
//
// Finally, the actual ARA code is agnostic to IPC being used, so when any ARA API
// call is made on any thread, the IPC implementation needs to figure out whether
// this call is part of a potentially ongoing transaction, or starting a new one.
// It does so by using thread local storage to indicate when a thread is currently
// processing a received message and is thus participating in an ongoing transaction.


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
