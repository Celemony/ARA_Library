//------------------------------------------------------------------------------
//! \file       ARAIPCConnection.cpp
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

#include "ARAIPCConnection.h"


#if ARA_ENABLE_IPC


#include "ARA_Library/Debug/ARADebug.h"
#include "ARA_Library/IPC/ARAIPCEncoding.h"

#include <utility>

#if defined (_WIN32)
    #include <Windows.h>
#elif defined (__APPLE__)
    #include <CoreFoundation/CoreFoundation.h>
#endif


// ARA IPC design overview
//
// ARA API calls can be stacked multiple times.
// One example for this is the creation of a new audio source: from the creation
// call or from the concluding endEditing(), the plug-in can call back into the host
// and query what content information it can provide for the newly created source.
// Another example with one more layer stacked can happen when the plug-in has completed
// analyzing an audio source. The next time the host calls notifyModelUpdates(), the
// plug-in will inform the host about the updated content information by invoking
// notifyAudioSourceContentChanged(). From that method, the host now queries
// isAudioSourceContentAvailable() for the relevant content types and then calls
// createAudioSourceContentReader() etc. to read the relevant content data.
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
// the incoming IPC messages on some undefined thread. In such cases, replies need
// to be dispatched from the receiving thread to the thread that is waiting for a
// particular reply.
// Each call message therefore includes a token that identifies the sending thread.
// The receiving side sends this token back with the reply message, allowing the
// threads in the sender to wait via a condition for any incoming replies and identify
// the matching reply via the token.
//
// Stacked calls on the other hand may need to be dispatched to some pre-allocated
// thread that can process the callback, so that the receive thread can return in
// order to no longer block the message channel, so that potentially further stacked
// calls can be sent across.
// As a result, code that was previously running e.g. on the main thread is now
// distributed across multiple threads, but of all theses threads only one is executing
// at any point in time. This decouples the logical flow of execution from the actual
// OS-level thread of execution, which is very similar to how e.g. Swift actors work.
// While this means that the high-level logic will keep working as-is, any test about
// being on a specific OS thread (usually the main thread) no longer work.
// Any such test now must include testing for the IPC threads, and API that has a
// hard requirement to e.g. be run on the main thread can no longer be used directly
// from ARA calls. Both hosts and plug-ins need to adapt to this, which can be quite
// challenging depending on the structure of the given code.
//
// While most ARA communication is happening on the main thread, there are several
// calls that may be made from other threads. This means that the IPC implementation
// must deal with thread concurrency, which introduces several additional challenges.
//
// First of all, there needs to be some locking mechanism around actually accessing
// the IPC. This adds a dependency between previously independent threads, so both
// hosts and plug-ins will need to carefully evaluate their existing ARA code to check
// for potential deadlocks or priority inversion caused by this.
// To reduce the potential impact of this, the implementation uses two IPC channels
// in parallel: one for all main thread communication, which can work lockless, and
// another one that is used for all other threads and uses a lock.
// When an ARA IPC call is made, the implementation checks whether this is happening
// on the main thread(s) or on any other thread, and chooses the appropriate channel
// accordingly.
// On the receiving side, calls coming in on the main thread channel are forwarded to
// the main channel thread pool, and for the other threads the code is executed directly
// on the receive thread.
// (Note that currently, stacking calls is only supported on the main thread in order
// to save resources - it could however be trivially enabled there as well if needed.)
//
// Further, calls that are using IPC are no longer realtime safe. This means that calls
// like getPlaybackRegionHeadAndTailTime(), which could previously be executed on render
// threads, now need to be moved to other threads. Accordingly, hosts need to cache data
// like the head and tail times from the main thread and update them there whenever
// notifyPlaybackRegionContentChanged() is received.


namespace ARA {
namespace IPC {


// key to store the threading information in the IPC messages
// when sending a message, this contains the sending thread. the receiving side stores this
// and sends it back with the matching reply so that the sending thread can be woken up.
constexpr MessageArgumentKey kSendThreadKey { -1 };


#if defined (_WIN32)
    #define readThreadRef readInt32
    #define appendThreadRef appendInt32
#else
    #define readThreadRef readSize
    #define appendThreadRef appendSize
#endif


// actually a "static" member of ARAIPCChannel, but for some reason C++ doesn't allow this...
thread_local bool _actsAsMainThread { false };


struct PendingReplyHandler
{
    Connection::ReplyHandler _replyHandler;
    void* _replyHandlerUserData;
};
// actually a "static" member of MessageChannel, but for some reason C++ doesn't allow this...
thread_local const PendingReplyHandler* _pendingReplyHandler { nullptr };


constexpr MessageDispatcher::ThreadRef MessageDispatcher::_invalidThread;


MessageDispatcher::MessageDispatcher (Connection* connection, MessageChannel* messageChannel, bool singleThreaded)
: _connection { connection },
  _messageChannel { messageChannel }
{
    static_assert (sizeof (std::thread::id) == sizeof (ThreadRef), "the current implementation relies on a specific thread ID size");
// unfortunately at least in clang std::thread::id's c'tor isn't constexpr
//  static_assert (std::thread::id {} == *reinterpret_cast<const std::thread::id*>(&_invalidThread), "the current implementation relies on invalid thread IDs being 0");
    ARA_INTERNAL_ASSERT (std::thread::id {} == *reinterpret_cast<const std::thread::id*>(&_invalidThread));
    
    _routedMessages.resize (12);    // we shouldn't use more than a handful of threads concurrently for the IPC

    if (singleThreaded)
        _addRecursionThread ();     // prepare for at least one level of recursion, this is typically enough
    else
        _sendLock = new std::mutex;

    _messageChannel->setMessageDispatcher (this);
}

MessageDispatcher::~MessageDispatcher ()
{
    if (_isSingleThreaded ())
    {
        _shutDown = true;
        _routeReceiveCondition.notify_all ();
        for (auto thread : _recursionThreads)
        {
            thread->join ();
            delete thread;
        }
    }
    else
    {
        delete _sendLock;
    }

    delete _messageChannel;
}

MessageDispatcher::ThreadRef MessageDispatcher::_getThreadRefForThreadID (std::thread::id threadID)
{
    const auto result { *reinterpret_cast<const ThreadRef*> (&threadID) };
    ARA_INTERNAL_ASSERT (result != _invalidThread);
    return result;
}

void MessageDispatcher::sendMessage (MessageID messageID, MessageEncoder* encoder,
                                     Connection::ReplyHandler replyHandler, void* replyHandlerUserData)
{
    const auto currentThread { _getCurrentThreadRef () };
    encoder->appendThreadRef (kSendThreadKey, currentThread);

    if (!_isSingleThreaded ())
        _sendLock->lock ();
    ARA_IPC_LOG ("sends message with ID %i on thread %p", messageID, currentThread);
    _messageChannel->sendMessage (messageID, encoder);
    if (!_isSingleThreaded ())
        _sendLock->unlock ();

    delete encoder;

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
        std::unique_lock <std::mutex> lock { _routeLock };
        _routeReceiveCondition.wait (lock, [this, &currentThread]
                                { return _getRoutedMessageForThread (currentThread) != nullptr; });
        RoutedMessage* message = _getRoutedMessageForThread (currentThread);
        ARA_INTERNAL_ASSERT (message->_messageID == 0);
        const auto receivedDecoder { message->_decoder };
        message->_targetThread = _invalidThread;
        lock.unlock ();

        _handleReply (receivedDecoder, replyHandler, replyHandlerUserData); // will also delete receivedDecoder
    }
}

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
    if (messageID != 0)
    {
        if (_isSingleThreaded ())
        {
            if (_nextRecursionThreadIndex == _recursionThreads.size ())
                _addRecursionThread ();
            auto targetThread { _getThreadRefForThreadID (_recursionThreads[_nextRecursionThreadIndex]->get_id ()) };
            ++_nextRecursionThreadIndex;

            ARA_IPC_LOG ("dispatches received message with ID %i from thread %p to handling thread %p", messageID, _getCurrentThreadRef (), targetThread);
            _routeMessageToThread (messageID, decoder, targetThread);
        }
        else
        {
            _handleReceivedMessage (messageID, decoder);
        }
    }
    else
    {
        ThreadRef targetThread;
        const auto success { decoder->readThreadRef (kSendThreadKey, &targetThread) };
        ARA_INTERNAL_ASSERT (success);
        ARA_INTERNAL_ASSERT (targetThread != _invalidThread);
        if (targetThread == _getCurrentThreadRef ())
        {
            ARA_INTERNAL_ASSERT (_messageChannel->runsReceiveLoopOnCurrentThread ());
            ARA_INTERNAL_ASSERT (_pendingReplyHandler != nullptr);
            _handleReply (decoder, _pendingReplyHandler->_replyHandler, _pendingReplyHandler->_replyHandlerUserData);
            _pendingReplyHandler = nullptr;
        }
        else
        {
            ARA_IPC_LOG ("dispatches received reply from thread %p to sending thread %p", _getCurrentThreadRef (), targetThread);
            _routeMessageToThread (0, decoder, targetThread);
        }
    }
}

void MessageDispatcher::_routeMessageToThread (MessageID messageID, const MessageDecoder* decoder, ThreadRef targetThread)
{
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

void MessageDispatcher::_handleReceivedMessage (MessageID messageID, const MessageDecoder* decoder)
{
    ARA_IPC_LOG ("handles received message with ID %i on thread %p", messageID, _getCurrentThreadRef ());
    ARA_INTERNAL_ASSERT (messageID != 0);

    auto replyEncoder { _connection->createEncoder () };

    ThreadRef remoteTargetThread;
    const auto success { decoder->readThreadRef (kSendThreadKey, &remoteTargetThread) };
    ARA_INTERNAL_ASSERT (success);
    replyEncoder->appendThreadRef (kSendThreadKey, remoteTargetThread);

    _connection->getMessageHandler ()->handleReceivedMessage (messageID, decoder, replyEncoder);
    delete decoder;

    if (!_isSingleThreaded ())
        _sendLock->lock ();
    ARA_IPC_LOG ("replies to message with ID %i on thread %p", messageID, _getCurrentThreadRef ());
    _messageChannel->sendMessage (0, replyEncoder);
    if (!_isSingleThreaded ())
        _sendLock->unlock ();

    delete replyEncoder;
}

void MessageDispatcher::_handleReply (const MessageDecoder* decoder, Connection::ReplyHandler replyHandler, void* replyHandlerUserData)
{
    ARA_IPC_LOG ("handles received reply on thread %p", _getCurrentThreadRef ());
    if (replyHandler)
        (replyHandler) (decoder, replyHandlerUserData);
    else
        ARA_INTERNAL_ASSERT (!decoder->hasDataForKey(0));   // replies should be empty when not handled (i.e. void)
    delete decoder;
}

void MessageDispatcher::_addRecursionThread ()
{
    _recursionThreads.emplace_back (new std::thread { [this] () {
        const auto currentThread { _getCurrentThreadRef () };
        _actsAsMainThread = true;
        while (true)
        {
            std::unique_lock <std::mutex> lock { _routeLock };
            _routeReceiveCondition.wait (lock, [this, &currentThread]
                                    { return (_getRoutedMessageForThread (currentThread) != nullptr) || _shutDown; });
            if (_shutDown)
                return;

            RoutedMessage* message = _getRoutedMessageForThread (currentThread);
            const auto receivedMessageID { message->_messageID };
            const auto receivedDecoder { message->_decoder };
            message->_targetThread = _invalidThread;
            lock.unlock ();

            ARA_INTERNAL_ASSERT (receivedMessageID != 0);
            _handleReceivedMessage (receivedMessageID, receivedDecoder);        // will also delete receivedDecoder

            ARA_INTERNAL_ASSERT (_nextRecursionThreadIndex != 0);
            --_nextRecursionThreadIndex;
        }
    } });
}

Connection::Connection ()
{
#if defined (__APPLE__)
    // since there is no way to create a dispatch queue associated with the current thread,
    // we require this is called on the main thread on Apple platforms, which has the only
    // well-defined dispatch queue on the system.
    ARA_INTERNAL_ASSERT (CFRunLoopGetMain () == CFRunLoopGetCurrent ());
#endif

    _actsAsMainThread = true;
}

Connection::~Connection ()
{
    delete _otherDispatcher;
    delete _mainDispatcher;
}

void Connection::setMainThreadChannel (MessageChannel* messageChannel)
{
    ARA_INTERNAL_ASSERT (_mainDispatcher == nullptr);
    _mainDispatcher = new MessageDispatcher { this, messageChannel, true };
}

void Connection::setOtherThreadsChannel (MessageChannel* messageChannel)
{
    ARA_INTERNAL_ASSERT (_otherDispatcher == nullptr);
    _otherDispatcher = new MessageDispatcher { this, messageChannel, false };
}

void Connection::setMessageHandler (MessageHandler* messageHandler)
{
    ARA_INTERNAL_ASSERT (_messageHandler == nullptr);
    _messageHandler = messageHandler;
}

void Connection::sendMessage (MessageID messageID, MessageEncoder* encoder, ReplyHandler replyHandler, void* replyHandlerUserData)
{
    ARA_INTERNAL_ASSERT ((_mainDispatcher != nullptr) && (_otherDispatcher != nullptr) && (_messageHandler != nullptr));
    if (_actsAsMainThread)
        _mainDispatcher->sendMessage (messageID, encoder, replyHandler, replyHandlerUserData);
    else
        _otherDispatcher->sendMessage (messageID, encoder, replyHandler, replyHandlerUserData);
}

bool Connection::currentThreadActsAsMainThread ()
{
    return _actsAsMainThread;
}

}   // namespace IPC
}   // namespace ARA

#endif // ARA_ENABLE_IPC
