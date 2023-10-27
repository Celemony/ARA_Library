//------------------------------------------------------------------------------
//! \file       ARAIPCAudioUnit_v3.m
//!             Implementation of ARA IPC message sending through AUMessageChannel
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

#import "ARAIPCAudioUnit_v3.h"


#if ARA_AUDIOUNITV3_IPC_IS_AVAILABLE


#import "ARA_Library/Debug/ARADebug.h"
#import "ARA_Library/IPC/ARAIPCEncoding.h"
#import "ARA_Library/IPC/ARAIPCCFEncoding.h"
#import "ARA_Library/IPC/ARAIPCProxyHost.h"
#import "ARA_Library/IPC/ARAIPCProxyPlugIn.h"
#if !ARA_ENABLE_IPC
    #error "configuration mismatch: enabling ARA_AUDIOUNITV3_IPC_IS_AVAILABLE requires enabling ARA_ENABLE_IPC too"
#endif

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>


namespace ARA {
namespace IPC {
extern "C" {


_Pragma ("GCC diagnostic push")
_Pragma ("GCC diagnostic ignored \"-Wold-style-cast\"") // __bridge casts can only be done old-style


API_AVAILABLE_BEGIN(macos(13.0))


#if 0
    #define ARA_IPC_LOG(...) ARA_LOG ("AUv3 IPC " __VA_ARGS__)
#else
    #define ARA_IPC_LOG(...) ((void) 0)
#endif


// custom IPC message to read the remote instance ref
constexpr auto kARAIPCGetRemoteInstanceRef { MethodID::createWithNonARAMethodID<-1> () };
// custom IPC message to force synchronous IPC shutdown despite the OS shutting down asynchronously
constexpr auto kARAIPCDestroyRemoteInstance { MethodID::createWithNonARAMethodID<-2> () };


// proxy host static handlers
ARAIPCAUBindingHandler _bindingHandler { nil };
ARAIPCAUDestructionHandler _destructionHandler { nil };


// key for transaction locking through the IPC channel
constexpr NSString * _transactionLockKey { @"transactionLock" };


// ARA AUv3 IPC design overview
//
// ARA API calls can be stacked multiple times.
// For example, when the plug-in has completed analyzing an audio source, it will
// inform the host about the updated content information the next time the host
// calls notifyModelUpdates() by invoking notifyAudioSourceContentChanged().
// From that method, the host now test which content types are available and
// eventually call createAudioSourceContentReader() to read the content data.
// In the code below, such a stack of messages is referred to as a "transaction".
//
// However, like many other IPC APIs, Audio Unit message channels cannot be stacked,
// i.e. each message has to completely be processed before the next one can be sent.
// Therefore, each ARA API calls is split into two IPC messages:
// the actual call message that does not return any result just yet, and a matching
// reply message that returns the result (even if it is void, because the original
// caller needs to know when the call has completed).
// After sending the actual call, the sender loops while listening for incoming
// messages, which might either be a stacked callback that is then processed
// accordingly, or the result of the original call.
//
// In this loop, extra efforts must be done to handle threading:
// Audio Unit message channels are using Grand Central Dispatch to deliver the
// incoming IPC messages, so in order to transfer them to the sending thread for
// proper handling, a concurrent queue must be used.
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
// Finally, the actual ARA code is agnostic to IPC being used, so when any API
// call is made on any thread, the IPC implementation needs to figure out whether
// this call is part of a potentially ongoing transaction, or starting a new one
// (in which case the thread must wait for the transaction lock).
// It does so by using thread local storage to indicate when a thread is currently
// processing a received message and is thus necessarily part of an ongoing
// transaction.
// Further, it checks if the same thread is already the currently sending thread,
// in which case it is part of a transaction stack, or it is a new transaction.


// simple multi-producer, multi-consumer lockless concurrent queue for incoming
// messages, based on a singly-linked list and a counting semaphore
class ConcurrentQueue
{
public:
    ConcurrentQueue ()
    : _semaphore { dispatch_semaphore_create (0) }
    {}

    ~ConcurrentQueue ()
    {
        dispatch_release (_semaphore);
    }

    void enqueueReceivedMessage (ARAIPCMessageID messageID, const ARAIPCMessageDecoder * decoder, void * userData)
    {
        auto newMessage { new ReceivedMessage { messageID, decoder, userData } };
        auto nextMessage { _receivedMessage.load (std::memory_order_relaxed) };
        while (true)
        {
            newMessage->_nextMessage.store (nextMessage, std::memory_order_relaxed);
            if (_receivedMessage.compare_exchange_weak (nextMessage, newMessage, std::memory_order_release, std::memory_order_relaxed))
            {
                dispatch_semaphore_signal (_semaphore);
                break;
            }
        }
    }

    std::tuple<ARAIPCMessageID, const ARAIPCMessageDecoder *, void *> dequeueReceivedMessage ()
    {
        dispatch_semaphore_wait (_semaphore, DISPATCH_TIME_FOREVER);

        auto current { &_receivedMessage };
        auto currentMessage { current->load (std::memory_order_consume) };
        if (!currentMessage)
        {
            ARA_INTERNAL_ASSERT (false);    // semaphore was set, so there must be data
            return { 0, nullptr, nullptr };
        }

        while (true)
        {
            auto next { &currentMessage->_nextMessage };
            auto nextMessage { next->load (std::memory_order_consume) };
            if (nextMessage == nullptr)
            {
                current->store (nullptr, std::memory_order_release);
                std::tuple<ARAIPCMessageID, const ARAIPCMessageDecoder *, void *> result { currentMessage->_messageID, currentMessage->_decoder, currentMessage->_userData };
                delete currentMessage;
                return result;
            }
            current = next;
            currentMessage = nextMessage;
        }
    }

private:
    struct ReceivedMessage
    {
        explicit ReceivedMessage (ARAIPCMessageID messageID, const ARAIPCMessageDecoder * decoder, void * userData)
        : _messageID { messageID },
          _decoder { decoder },
          _userData { userData }
        {}

        const ARAIPCMessageID _messageID;
        const ARAIPCMessageDecoder * const _decoder;
        void * const _userData;
        std::atomic<ReceivedMessage *> _nextMessage { nullptr };
    };

    std::atomic<ReceivedMessage *> _receivedMessage { nullptr };
    dispatch_semaphore_t _semaphore;
};


// base class for both proxy implementations

thread_local bool _isReceivingOnThisThread { false };   // actually a "static" member of ARAIPCAUMessageSender, but for some reason C++ doesn't allow this...

class ARAIPCAUMessageSender : public ARAIPCMessageSender
{
public:
    ARAIPCAUMessageSender (NSObject<AUMessageChannel> * _Nonnull messageChannel)
    : _messageChannel { messageChannel }
    {
#if !__has_feature(objc_arc)
        [_messageChannel retain];
#endif
    }

#if !__has_feature(objc_arc)
    ~ARAIPCAUMessageSender () override
    {
        [_messageChannel release];
    }
#endif

    NSObject<AUMessageChannel> * _Nonnull getMessageChannel ()
    {
        return _messageChannel;
    }

    ARAIPCMessageEncoder * createEncoder () override
    {
        return ARAIPCCFCreateMessageEncoder ();
    }

    bool receiverEndianessMatches () override
    {
        // \todo shouldn't the AUMessageChannel provide this information?
        return true;
    }

    void sendMessage (ARAIPCMessageID messageID, ARAIPCMessageEncoder * encoder,
                      ARAIPCReplyHandler * replyHandler, void * replyHandlerUserData) override
    {
        @autoreleasepool
        {
            const auto thisThread { std::this_thread::get_id () };
            const auto previousSendingThread { _sendingThread.load (std::memory_order_acquire) };
            const bool isNewTransaction { (thisThread != previousSendingThread) && !_isReceivingOnThisThread };

            // \todo can we do better here than spinning/sleeping here? And for how long?
            if (isNewTransaction)
                while (!_tryLockTransaction ())
                    std::this_thread::sleep_for (std::chrono::microseconds { 100 });

            _sendingThread.store (thisThread, std::memory_order_release);

            PendingReply pendingReply { replyHandler, replyHandlerUserData, _pendingReply.load (std::memory_order_acquire) };
            _pendingReply.store (&pendingReply, std::memory_order_release);

            ARA_IPC_LOG ("sends message %i%s", messageID, (isNewTransaction) ? " (starting new transaction)" : "");

            NSDictionary * message { CFBridgingRelease (ARAIPCCFCopyMessageEncoderDictionaryAddingMessageID (encoder, messageID)) };
            _sendMessage (message);

            do
            {
                const auto receivedMessage { _receiveQueue.dequeueReceivedMessage () };
                _processReceivedMessage (std::get<0> (receivedMessage), std::get<1> (receivedMessage), std::get<2> (receivedMessage));
            } while (_pendingReply.load (std::memory_order_acquire) == &pendingReply);

            ARA_IPC_LOG ("received reply to message %i%s", messageID, (isNewTransaction) ? " (ending transaction)" : "");

            _sendingThread.store (previousSendingThread, std::memory_order_release);

            if (isNewTransaction)
                _unlockTransaction ();
        }
    }

    virtual NSDictionary * processReceivedMessage (NSDictionary * message, void * userData)
    {
        const auto decoder { ARAIPCCFCreateMessageDecoderWithDictionary ((__bridge CFDictionaryRef) message) };
        const ARAIPCMessageID messageID { ARAIPCCFGetMessageIDFromDictionary (decoder) };

        if (_pendingReply.load (std::memory_order_acquire) != nullptr)
        {
            //ARA_IPC_LOG ("processReceivedMessage enqueues for sending thread");
            _receiveQueue.enqueueReceivedMessage (messageID, decoder, userData);
        }
        else
        {
            if (const auto dispatchTarget { _getDispatchTargetForIncomingTransaction (messageID) })
            {
                //ARA_IPC_LOG ("processReceivedMessage dispatches");
                dispatch_async (dispatchTarget,
                    ^{
                        //ARA_IPC_LOG ("processReceivedMessage processes dispatched");
                        _processReceivedMessage (messageID, decoder, userData);
                    });
            }
            else
            {
                //ARA_IPC_LOG ("processReceivedMessage processes directly");
                _processReceivedMessage (messageID, decoder, userData);
            }
        }

        delete decoder;

        return [NSDictionary dictionary];  // \todo it would yield better performance if the callHostBlock would allow nil as return value
    }

protected:
    virtual bool _tryLockTransaction () = 0;
    virtual void _unlockTransaction () = 0;
    virtual NSDictionary * _sendMessage (NSDictionary * message) = 0;
    virtual dispatch_queue_t _getDispatchTargetForIncomingTransaction (ARAIPCMessageID messageID) = 0;
    virtual void _handleReceivedMessage (const ARAIPCMessageID messageID,
                                         const ARAIPCMessageDecoder * const decoder,
                                         void * const userData,
                                         ARAIPCMessageEncoder * const replyEncoder) = 0;

private:
    void _processReceivedMessage (ARAIPCMessageID messageID, const ARAIPCMessageDecoder * decoder, void * userData)
    {
        const bool isAlreadyReceiving { _isReceivingOnThisThread };
        if (!isAlreadyReceiving)
            _isReceivingOnThisThread = true;

        if (messageID != 0)
        {
            ARA_IPC_LOG ("received message with ID %i%s", messageID, (_pendingReply.load (std::memory_order_relaxed) != nullptr) ? " (while awaiting reply)" : "");

            auto replyEncoder { createEncoder () };
            _handleReceivedMessage (messageID, decoder, userData, replyEncoder);
            NSDictionary * reply { CFBridgingRelease (ARAIPCCFCopyMessageEncoderDictionaryAddingMessageID (replyEncoder, 0)) };
            delete replyEncoder;

            ARA_IPC_LOG ("replies to message with ID %i", messageID);
            _sendMessage (reply);
        }
        else
        {
            const auto pendingReply { _pendingReply.load (std::memory_order_acquire) };
            ARA_INTERNAL_ASSERT (pendingReply != nullptr);
            if (pendingReply->replyHandler)
                (*pendingReply->replyHandler) (decoder, pendingReply->replyHandlerUserData);
            else
                ARA_INTERNAL_ASSERT (decoder->isEmpty ());  // unused replies should be empty
            _pendingReply.store (pendingReply->previousPendingReply, std::memory_order_release);
        }
 
        delete decoder;

        if (!isAlreadyReceiving)
            _isReceivingOnThisThread = false;
    }

private:
    struct PendingReply
    {
        ARAIPCReplyHandler * replyHandler;
        void * replyHandlerUserData;
        PendingReply * previousPendingReply;
    };

    NSObject<AUMessageChannel> * __strong _messageChannel;
    ConcurrentQueue _receiveQueue;
    std::atomic <std::thread::id> _sendingThread {};
    std::atomic<PendingReply *> _pendingReply {};   // if set, the receive callback forward to that thread
};


// plug-in side: proxy host implementation
class ARAIPCAUHostMessageSender : public ARAIPCAUMessageSender
{
public:
    ARAIPCAUHostMessageSender (NSObject<AUMessageChannel> * _Nonnull messageChannel)
    : ARAIPCAUMessageSender { messageChannel },
      _sendLock { dispatch_semaphore_create (1) }
    {}

#if !__has_feature(objc_arc)
    ~ARAIPCAUHostMessageSender () override
    {
        dispatch_release (_sendLock);
    }
#endif

protected:
    NSDictionary * _sendMessage (NSDictionary * message) override
    {
        if (const auto callHostBlock { getMessageChannel ().callHostBlock })
        {
            dispatch_semaphore_wait (_sendLock, DISPATCH_TIME_FOREVER);
            NSDictionary * reply { callHostBlock (message) };
            dispatch_semaphore_signal (_sendLock);
            ARA_INTERNAL_ASSERT (([reply count] == 0) || (([reply count] == 1) && [message objectForKey:_transactionLockKey]));
            return reply;
        }
        else
        {
            ARA_INTERNAL_ASSERT (false && "trying to send IPC message while host has not set callHostBlock");
            return nil;
        }
    }

    dispatch_queue_t _getDispatchTargetForIncomingTransaction (ARAIPCMessageID /*messageID*/) override
    {
        return ([NSThread isMainThread]) ? nullptr : dispatch_get_main_queue ();
    }

    void _handleReceivedMessage (const ARAIPCMessageID messageID,
                                 const ARAIPCMessageDecoder * const decoder,
                                 void * const userData,
                                 ARAIPCMessageEncoder * const replyEncoder) override
    {
        if (messageID == kARAIPCGetRemoteInstanceRef)
        {
            ARA_INTERNAL_ASSERT (userData != nullptr);
            ARA::IPC::encodeReply (replyEncoder, (ARAIPCPlugInInstanceRef) userData);
        }
        else if (messageID == kARAIPCDestroyRemoteInstance)
        {
            ARA_INTERNAL_ASSERT (!decoder->isEmpty ());
            ARAIPCPlugInInstanceRef plugInInstanceRef;
            decodeArguments (decoder, plugInInstanceRef);

            // \todo as long as we're using a separate channel per AUAudioUnit, we don't need to
            //       send the remote instance - it's known in the channel and provided as arg here
            ARA_INTERNAL_ASSERT ((void *) plugInInstanceRef == userData);
            _destructionHandler ((__bridge AUAudioUnit *) (void *) plugInInstanceRef);
        }
        else
        {
            ARAIPCProxyHostCommandHandler (messageID, decoder, replyEncoder);
        }
    }
    
    bool _tryLockTransaction () override
    {
        const auto message { [NSDictionary dictionaryWithObject:[NSNumber numberWithBool:true] forKey:_transactionLockKey] };
        const auto reply { _sendMessage (message) };
        ARA_INTERNAL_ASSERT ([reply objectForKey:_transactionLockKey] != nil);
        const auto result { [(NSNumber *) [reply objectForKey:_transactionLockKey] boolValue] };
        if (result)
            std::atomic_thread_fence (std::memory_order_acquire);
        return result;
    }

    void _unlockTransaction () override
    {
        std::atomic_thread_fence (std::memory_order_release);
        const auto message { [NSDictionary dictionaryWithObject:[NSNumber numberWithBool:false] forKey:_transactionLockKey] };
        const auto reply { _sendMessage (message) };
        ARA_INTERNAL_ASSERT ([reply objectForKey:_transactionLockKey] != nil);
        ARA_INTERNAL_ASSERT ([(NSNumber *) [reply objectForKey:_transactionLockKey] boolValue]);
    }

private:
    dispatch_semaphore_t _sendLock;     // needed because we're injecting messages from any thread in order to access the transaction lock
};


// host side: proxy plug-in implementation
class ARAIPCAUPlugInMessageSender : public ARAIPCAUMessageSender
{
public:
    ARAIPCAUPlugInMessageSender (NSObject<AUMessageChannel> * _Nonnull messageChannel)
    : ARAIPCAUMessageSender { messageChannel },
      _transactionLock { dispatch_semaphore_create (1) }
    {
        // \todo we happen to use the same message block for both the ARA_AUDIOUNIT_FACTORY_CUSTOM_MESSAGES_UTI
        //       and all ARA_AUDIOUNIT_PLUGINEXTENSION_CUSTOM_MESSAGES_UTI uses, but it is generally unclear
        //       how to set the correct message block if the channel is shared.
        getMessageChannel ().callHostBlock =
            ^NSDictionary * _Nullable (NSDictionary * _Nonnull message)
            {
                return processReceivedMessage (message, nullptr);
            };
    }

    ~ARAIPCAUPlugInMessageSender () override
    {
        getMessageChannel ().callHostBlock = nil;
#if !__has_feature(objc_arc)
        dispatch_release (_transactionLock);
#endif
    }

protected:
    NSDictionary * _sendMessage (NSDictionary * message) override
    {
        NSDictionary * reply { [getMessageChannel () callAudioUnit:message] };
        ARA_INTERNAL_ASSERT (([reply count] == 0) || (([reply count] == 1) && [message objectForKey:_transactionLockKey]));
        return reply;
    }

    dispatch_queue_t _getDispatchTargetForIncomingTransaction (ARAIPCMessageID messageID) override
    {
        ARA_INTERNAL_ASSERT((messageID == ARA_IPC_HOST_METHOD_ID (ARAAudioAccessControllerInterface, readAudioSamples).getMessageID ()) ||
                            ((ARA_IPC_HOST_METHOD_ID (ARAPlaybackControllerInterface, requestStartPlayback).getMessageID () <= messageID) &&
                             (messageID <= ARA_IPC_HOST_METHOD_ID (ARAPlaybackControllerInterface, requestEnableCycle).getMessageID ())));
        return nullptr;
    }

    void _handleReceivedMessage (const ARAIPCMessageID messageID,
                                 const ARAIPCMessageDecoder * const decoder,
                                 void * const /*userData*/,
                                 ARAIPCMessageEncoder * const replyEncoder) override
    {
        ARAIPCProxyPlugInCallbacksDispatcher (messageID, decoder, replyEncoder);
    }

    bool _tryLockTransaction () override
    {
        return (dispatch_semaphore_wait (_transactionLock, DISPATCH_TIME_NOW) == 0);
    }

    void _unlockTransaction () override
    {
        dispatch_semaphore_signal (_transactionLock);
    }

    NSDictionary * processReceivedMessage (NSDictionary * message, void * userData) override
    {
        if (const auto transactionLockRequest { (NSNumber *) [message objectForKey:_transactionLockKey] })
        {
            bool result { true };
            if ([transactionLockRequest boolValue])
                result = _tryLockTransaction ();
            else
                _unlockTransaction ();
            return [NSDictionary dictionaryWithObject:[NSNumber numberWithBool:result] forKey:_transactionLockKey];
        }

        return ARAIPCAUMessageSender::processReceivedMessage (message, userData);
    }

private:
    dispatch_semaphore_t _transactionLock;
};


// host side: proxy plug-in specialization for plug-in extension messages
class ARAIPCAUPlugInExtensionMessageSender : public ARAIPCAUPlugInMessageSender
{
    using ARAIPCAUPlugInMessageSender::ARAIPCAUPlugInMessageSender;

public:
    static ARAIPCAUPlugInExtensionMessageSender * bindAndCreateMessageSender (NSObject<AUMessageChannel> * _Nonnull messageChannel,
                                                                              ARADocumentControllerRef _Nonnull documentControllerRef,
                                                                              ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles)
    {
        auto result { new ARAIPCAUPlugInExtensionMessageSender { messageChannel } };
        ARA::IPC::RemoteCaller { result }.remoteCall (result->_remoteInstanceRef, kARAIPCGetRemoteInstanceRef);
        result->_plugInExtensionInstance = ARAIPCProxyPlugInBindToDocumentController (result->_remoteInstanceRef, result, documentControllerRef, knownRoles, assignedRoles);
        return result;
    }

    ~ARAIPCAUPlugInExtensionMessageSender () override
    {
        if (_plugInExtensionInstance)
        {
            RemoteCaller { this }.remoteCall (kARAIPCDestroyRemoteInstance, _remoteInstanceRef);
            ARAIPCProxyPlugInCleanupBinding (_plugInExtensionInstance);
        }
    }

    const ARAPlugInExtensionInstance * getPlugInExtensionInstance () const
    {
        return _plugInExtensionInstance;
    }

private:
    ARAIPCPlugInInstanceRef _remoteInstanceRef {};                  // stores the remote AUAudioUnit<ARAAudioUnit> instance
    const ARAPlugInExtensionInstance * _plugInExtensionInstance {};
};



// host side: proxy plug-in C adapter to ARAIPCAUPlugInMessageSender (or ARAIPCAUPlugInExtensionMessageSender subclass)
NSObject<AUMessageChannel> * _Nullable ARAIPCAUGetMessageChannel (AUAudioUnit * _Nonnull audioUnit, NSString * _Nonnull identifier)
{
    // AUAudioUnits created before macOS 13 will not know about this API yet
    if (![audioUnit respondsToSelector:@selector(messageChannelFor:)])
        return nil;

    return (NSObject<AUMessageChannel> *) [(AUAudioUnit<ARAAudioUnit> *) audioUnit messageChannelFor:identifier];
}

ARAIPCMessageSender * _Nullable ARA_CALL ARAIPCAUProxyPlugInInitializeFactoryMessageSender (AUAudioUnit * _Nonnull audioUnit)
{
    auto messageChannel { ARAIPCAUGetMessageChannel (audioUnit, ARA_AUDIOUNIT_FACTORY_CUSTOM_MESSAGES_UTI) };
    if (!messageChannel)
        return nullptr;

    return new ARAIPCAUPlugInMessageSender { (NSObject<AUMessageChannel> * _Nonnull) messageChannel };
}

const ARAFactory * _Nonnull ARA_CALL ARAIPCAUProxyPlugInGetFactory (ARAIPCMessageSender * _Nonnull messageSender)
{
    ARA_VALIDATE_API_CONDITION (ARAIPCProxyPlugInGetFactoriesCount (messageSender) == 1);
    const ARAFactory * result { ARAIPCProxyPlugInGetFactoryAtIndex (messageSender, 0) };
    ARA_VALIDATE_API_CONDITION (result != nullptr);
    return result;
}

const ARAPlugInExtensionInstance * _Nullable ARA_CALL ARAIPCAUProxyPlugInBindToDocumentController (AUAudioUnit * _Nonnull audioUnit,
                                                                                                   ARADocumentControllerRef _Nonnull documentControllerRef,
                                                                                                   ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles,
                                                                                                   ARAIPCMessageSender * _Nullable * _Nonnull messageSender)
{
    auto messageChannel { ARAIPCAUGetMessageChannel (audioUnit, ARA_AUDIOUNIT_PLUGINEXTENSION_CUSTOM_MESSAGES_UTI) };
    if (!messageChannel)
    {
        *messageSender = nullptr;
        return nullptr;
    }

    *messageSender = ARAIPCAUPlugInExtensionMessageSender::bindAndCreateMessageSender ((NSObject<AUMessageChannel> * _Nonnull) messageChannel,
                                                                                       documentControllerRef, knownRoles, assignedRoles);
    return static_cast<ARAIPCAUPlugInExtensionMessageSender *> (*messageSender)->getPlugInExtensionInstance ();
}

void ARA_CALL ARAIPCAUProxyPlugInCleanupBinding (ARAIPCMessageSender * messageSender)
{
    delete messageSender;
}

void ARA_CALL ARAIPCAUProxyPlugInUninitializeFactoryMessageSender (ARAIPCMessageSender * _Nonnull messageSender)
{
    delete messageSender;
}



// plug-in side: proxy host C adapter to ARAIPCAUHostMessageSender
ARAIPCAUHostMessageSender * _sharedProxyHostCallbacksSender {};

void ARA_CALL ARAIPCAUProxyHostAddFactory (const ARAFactory * _Nonnull factory)
{
    ARAIPCProxyHostAddFactory (factory);
}

const ARAPlugInExtensionInstance * ARA_CALL ARAIPCAUBindingHandlerWrapper (ARAIPCPlugInInstanceRef plugInInstanceRef, ARADocumentControllerRef controllerRef,
                                                                           ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles)
{
    auto audioUnit { (__bridge AUAudioUnit *) (void *) plugInInstanceRef };
    return _bindingHandler (audioUnit, controllerRef, knownRoles, assignedRoles);
}

void ARA_CALL ARAIPCAUProxyHostInitialize (NSObject<AUMessageChannel> * _Nonnull factoryMessageChannel, ARAIPCAUBindingHandler _Nonnull bindingHandler, ARAIPCAUDestructionHandler _Nonnull destructionHandler)
{
    _sharedProxyHostCallbacksSender = new ARAIPCAUHostMessageSender { factoryMessageChannel };
    ARAIPCProxyHostSetPlugInCallbacksSender (_sharedProxyHostCallbacksSender);

#if __has_feature(objc_arc)
    _bindingHandler = bindingHandler;
    _destructionHandler = destructionHandler;
#else
    _bindingHandler = [bindingHandler retain];
    _destructionHandler = [destructionHandler retain];
#endif
    ARAIPCProxyHostSetBindingHandler (ARAIPCAUBindingHandlerWrapper);
}

NSDictionary * _Nonnull ARA_CALL ARAIPCAUProxyHostCommandHandler (AUAudioUnit * _Nullable audioUnit, NSDictionary * _Nonnull message)
{
    return _sharedProxyHostCallbacksSender->processReceivedMessage (message, (__bridge void *)audioUnit);
}

void ARA_CALL ARAIPCAUProxyHostCleanupBinding (const ARAPlugInExtensionInstance * _Nonnull plugInExtensionInstance)
{
    ARAIPCProxyHostCleanupBinding (plugInExtensionInstance);
}

void ARA_CALL ARAIPCAUProxyHostUninitialize (void)
{
#if __has_feature(objc_arc)
    _bindingHandler = nil;
    _destructionHandler = nil;
#else
    [_bindingHandler release];
    [_destructionHandler release];
#endif
    delete _sharedProxyHostCallbacksSender;
}


API_AVAILABLE_END


_Pragma ("GCC diagnostic pop")


}   // extern "C"
}   // namespace IPC
}   // namespace ARA


#endif // ARA_AUDIOUNITV3_IPC_IS_AVAILABLE
