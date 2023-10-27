//------------------------------------------------------------------------------
//! \file       ARAIPCMultiThreadedChannel.cpp
//!             Base class implementation for both the ARA IPC proxy host and plug-in
//!             Typically, this file is not included directly - either ARAIPCProxyHost.h
//!             ARAIPCProxyPlugIn.h will be used instead.
//! \project    ARA SDK Library
//! \copyright  Copyright (c) 2021-2023, Celemony Software GmbH, All Rights Reserved.
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

#include "ARAIPCMultiThreadedChannel.h"


#if ARA_ENABLE_IPC


#include "ARA_Library/Debug/ARADebug.h"
#include "ARA_Library/IPC/ARAIPCEncoding.h"
#include "ARA_Library/IPC/ARAIPCProxyHost.h"
#include "ARA_Library/IPC/ARAIPCProxyPlugIn.h"

#include <utility>

#include <CoreFoundation/CoreFoundation.h>


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
// transferred to the sending thread for proper handling using a concurrent queue.
//
// While most ARA communication is happening on the main thread, there are
// some calls that may be made from other threads. This poses several challenges
// when tunneling everything through a single IPC channel.
// First of all, there needs to be a global lock to handle access to the channel
// from either some thread in the host or in the plug-in. This lock must be
// recursive to allow the stacking of messages as an atomic "transaction" as
// described above. It is implemented as a regular lock in the host, which the
// plug-in can access it by injecting special tryLock/unlock messages into the
// IPC communication.
// So, if in the above content reading example there is a additional concurrent
// access from the plug-in to the host because analysis of another audio source
// is still going on in the background, the message sequence might look like this:
//   - host takes transaction lock locally
//   - host sends notifyAudioSourceContentChanged message
//     - plug-in sends notifyAudioSourceContentChanged message
//       - host sends isAudioSourceContentAvailable message
//       - plug-in sends reply to isAudioSourceContentAvailable message
//       - ... more content reading here ...
//     - host sends reply to notifyAudioSourceContentChanged message
//   - plug-in sends reply to notifyAudioSourceContentChanged message
//   - host releases transaction lock locally
//   - plug-in takes transaction lock in host via remote message
//     - plug-in sends readAudioSamples message
//     - host sends reply to readAudioSamples message
//   - plug-in releases transaction lock in host via remote message
//
// Another challenge when using IPC between host and plug-in is that for each
// new transaction that comes in, an appropriate thread has to be selected to
// process the transaction. Fortunately, the current threading restrictions of
// ARA allow for a fairly simple pattern to address this:
// All transactions that the host initiates are processed on the plug-in's main
// thread because the vast majority of calls is restricted to the main thread
// anyways, and the few calls that may be made on other threads (such as
// getPlaybackRegionHeadAndTailTime()) are all allowed on the main thread too.
// All transactions started from the plug-in (readAudioSamples() and the
// functions in ARAPlaybackControllerInterface) are calls that can come from
// any thread and thus are directly processed on the IPC thread in the host.
// (Remember, the side that started a transaction will see all calls on the
// thread that started it.)
//
// Finally, the actual ARA code is agnostic to IPC being used, so when any ARA API
// call is made on any thread, the IPC implementation needs to figure out whether
// this call is part of a potentially ongoing transaction, or starting a new one
// (in which case the thread must wait for the transaction lock).
// It does so by using thread local storage to indicate when a thread is currently
// processing a received message and is thus participating in an ongoing transaction.
// Further, it checks if the same thread is already the currently sending thread,
// in which case it is part of a transaction stack, or it is a new transaction.


#if 0
    #define ARA_IPC_LOG(...) ARA_LOG ("ARA IPC " __VA_ARGS__)
#else
    #define ARA_IPC_LOG(...) ((void) 0)
#endif


namespace ARA {
namespace IPC {


// simple thread bridge to move a single incoming message from a receiving to a sending thread
class SendThreadBridge
{
public:
    SendThreadBridge ()
    : _semaphore { dispatch_semaphore_create (0) }
    {
        ARA_INTERNAL_ASSERT (_semaphore != nullptr);
    }

#if !__has_feature(objc_arc)
    ~SendThreadBridge ()
    {
        dispatch_release (_semaphore);
    }
#endif

    // called on receive thread
    void pushReceivedMessage (ARAIPCMessageID messageID, const ARAIPCMessageDecoder* decoder)
    {
        _messageID = messageID;
        _decoder = decoder;
        std::atomic_thread_fence (std::memory_order_release);
        dispatch_semaphore_signal (_semaphore);
    }

    // called on send thread
    void pullReceivedMessage (ARAIPCMessageID& messageID, const ARAIPCMessageDecoder*& decoder)
    {
        dispatch_semaphore_wait (_semaphore, DISPATCH_TIME_FOREVER);
        std::atomic_thread_fence (std::memory_order_acquire);
        messageID = _messageID;
        decoder = _decoder;
    }

private:
    dispatch_semaphore_t _semaphore;
    ARAIPCMessageID _messageID { 0 };
    const ARAIPCMessageDecoder* _decoder { nullptr };
};


// plug-in side implementation of ARAIPCMessageHandler

std::thread::id _getMainThreadID ()
{
    if (CFRunLoopGetMain () == CFRunLoopGetCurrent ())
    {
        return std::this_thread::get_id ();
    }
    else
    {
        __block std::thread::id result;
        dispatch_sync (dispatch_get_main_queue (),
                        ^{
                            result = std::this_thread::get_id ();
                        });
        return result;
    }
}

ARAIPCProxyHostMessageHandler::ARAIPCProxyHostMessageHandler ()
: _mainThreadID { _getMainThreadID () },
  _mainThreadDispatchTarget { dispatch_get_main_queue () }
{}

ARAIPCMessageHandler::DispatchTarget ARAIPCProxyHostMessageHandler::getDispatchTargetForIncomingTransaction (ARAIPCMessageID /*messageID*/)
{
    return (std::this_thread::get_id () == _mainThreadID) ? nullptr : _mainThreadDispatchTarget;
}

void ARAIPCProxyHostMessageHandler::handleReceivedMessage (ARAIPCMessageChannel* messageChannel,
                                                           const ARAIPCMessageID messageID, const ARAIPCMessageDecoder* const decoder,
                                                           ARAIPCMessageEncoder* const replyEncoder)
{
    ARAIPCProxyHostCommandHandler (messageChannel, messageID, decoder, replyEncoder);
}


// host side implementation of ARAIPCMessageHandler
ARAIPCMessageHandler::DispatchTarget ARAIPCProxyPlugInMessageHandler::getDispatchTargetForIncomingTransaction (ARAIPCMessageID messageID)
{
    ARA_INTERNAL_ASSERT ((messageID == ARA_IPC_HOST_METHOD_ID (ARAAudioAccessControllerInterface, readAudioSamples).getMessageID ()) ||
                         ((ARA_IPC_HOST_METHOD_ID (ARAPlaybackControllerInterface, requestStartPlayback).getMessageID () <= messageID) &&
                          (messageID <= ARA_IPC_HOST_METHOD_ID (ARAPlaybackControllerInterface, requestEnableCycle).getMessageID ())));
    return nullptr;
}

void ARAIPCProxyPlugInMessageHandler::handleReceivedMessage (ARAIPCMessageChannel* messageChannel,
                                                             const ARAIPCMessageID messageID, const ARAIPCMessageDecoder* const decoder,
                                                             ARAIPCMessageEncoder* const replyEncoder)
{
    ARAIPCProxyPlugInCallbacksDispatcher (messageChannel, messageID, decoder, replyEncoder);
}


// generic thread handling for multi-threaded channels (i.e. messages are note received on the sending thread)

thread_local bool _isReceivingOnThisThread { false };   // actually a "static" member of MultiThreadedChannel, but for some reason C++ doesn't allow this...

MultiThreadedChannel::MultiThreadedChannel (ARAIPCMessageHandler* handler)
: _handler { handler },
  _sendThreadBridge { new SendThreadBridge }
{}

MultiThreadedChannel::~MultiThreadedChannel ()
{
    delete _sendThreadBridge;
}

void MultiThreadedChannel::sendMessage (ARAIPCMessageID messageID, ARAIPCMessageEncoder* encoder,
                                        ReplyHandler replyHandler, void* replyHandlerUserData)
{
    const auto thisThread { std::this_thread::get_id () };
    const auto previousSendingThread { _sendingThread.load (std::memory_order_acquire) };
    const bool isNewTransaction { (thisThread != previousSendingThread) && !_isReceivingOnThisThread };

    if (isNewTransaction)
        lockTransaction ();

    _sendingThread.store (thisThread, std::memory_order_release);

    ARA_IPC_LOG ("sends message %i%s", messageID, (isNewTransaction) ? " (starting new transaction)" : "");
    _sendMessage (messageID, encoder);
    delete encoder;

    bool didReceiveReply { false };
    do
    {
        ARAIPCMessageID receivedMessageID;
        const ARAIPCMessageDecoder* receivedDecoder;
        _sendThreadBridge->pullReceivedMessage (receivedMessageID, receivedDecoder);
        if (receivedMessageID != 0)
        {
            _handleReceivedMessage (receivedMessageID, receivedDecoder);    // will also delete decoder
        }
        else
        {
            if (replyHandler)
                (replyHandler) (receivedDecoder, replyHandlerUserData);
            else
                ARA_INTERNAL_ASSERT (!receivedDecoder);                     // replies should be empty when not handled (i.e. void)
            delete receivedDecoder;
            didReceiveReply = true;
        }
    }
    while (!didReceiveReply);

    ARA_IPC_LOG ("received reply to message %i%s", messageID, (isNewTransaction) ? " (ending transaction)" : "");

    _sendingThread.store (previousSendingThread, std::memory_order_release);

    if (isNewTransaction)
        unlockTransaction ();
}

void MultiThreadedChannel::routeReceivedMessage (ARAIPCMessageID messageID, const ARAIPCMessageDecoder* decoder)
{
    if (_sendingThread.load (std::memory_order_acquire) != std::thread::id {})
    {
        if (messageID != 0)
            ARA_IPC_LOG ("dispatches received message with ID %i to sending thread", messageID);
        else
            ARA_IPC_LOG ("dispatches received reply to sending thread", messageID);
        _sendThreadBridge->pushReceivedMessage (messageID, decoder);
    }
    else
    {
        ARA_INTERNAL_ASSERT (messageID != 0);
        if (const auto dispatchTarget { _handler->getDispatchTargetForIncomingTransaction (messageID) })
        {
            ARA_IPC_LOG ("dispatches received message with ID %i (new transaction)", messageID);
            dispatch_async (dispatchTarget,
                ^{
                    _handleReceivedMessage (messageID, decoder);
                });
        }
        else
        {
            ARA_IPC_LOG ("directly handles received message with ID %i (new transaction)", messageID);
            _handleReceivedMessage (messageID, decoder);
        }
    }
}

void MultiThreadedChannel::_handleReceivedMessage (ARAIPCMessageID messageID, const ARAIPCMessageDecoder* decoder)
{
    ARA_INTERNAL_ASSERT (messageID != 0);

    const bool isAlreadyReceiving { _isReceivingOnThisThread };
    if (!isAlreadyReceiving)
        _isReceivingOnThisThread = true;

    auto replyEncoder { createEncoder () };
//  ARA_IPC_LOG ("handles message with ID %i%s", messageID,
//                      (_sendingThread.load (std::memory_order_acquire) != std::thread::id {}) ? " (while awaiting reply)" : "");
    _handler->handleReceivedMessage (this, messageID, decoder, replyEncoder);
    delete decoder;

    ARA_IPC_LOG ("replies to message with ID %i", messageID);
    _sendMessage (0, replyEncoder);
    delete replyEncoder;

    if (!isAlreadyReceiving)
        _isReceivingOnThisThread = false;
}

}   // namespace IPC
}   // namespace ARA

#endif // ARA_ENABLE_IPC
