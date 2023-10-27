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


#if defined(__cplusplus)
namespace ARA {
namespace IPC {
extern "C" {
#endif


_Pragma ("GCC diagnostic push")
_Pragma ("GCC diagnostic ignored \"-Wold-style-cast\"") // __bridge casts can only be done old-style


API_AVAILABLE_BEGIN(macos(13.0))


// custom IPC message to read the remote instance ref
constexpr auto kARAIPCGetRemoteInstanceRef { MethodID::createWithNonARAMethodID<-1> () };
// custom IPC message to force synchronous IPC shutdown despite the OS shutting down asynchronously
constexpr auto kARAIPCDestroyRemoteInstance { MethodID::createWithNonARAMethodID<-2> () };


// proxy host static handlers
ARAIPCAUBindingHandler _bindingHandler { nil };
ARAIPCAUDestructionHandler _destructionHandler { nil };


struct ReceivedMessage
{
    explicit ReceivedMessage (NSDictionary * message, void * userData)
    : _message { message },
      _userData { userData }
    {
#if !__has_feature(objc_arc)
        [message retain];
#endif
    }
#if !__has_feature(objc_arc)
    ~ReceivedMessage ()
    {
        [_message release];
    }
#endif

    NSDictionary * _message;
    void * _userData;
    std::atomic<ReceivedMessage *> _nextMessage { nullptr };
};


class ARAIPCAUMessageSender : public ARAIPCMessageSender
{
public:
    ARAIPCAUMessageSender (NSObject<AUMessageChannel>* _Nonnull messageChannel)
    : _messageChannel { messageChannel },
      _receiveSemaphore { dispatch_semaphore_create (0) }
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

    ARAIPCMessageEncoder* createEncoder () override
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
        if (![NSThread isMainThread])
        {
            dispatch_sync (dispatch_get_main_queue (),
            ^{
                sendMessage (messageID, encoder, replyHandler, replyHandlerUserData);
            });
            return;
        }

        @autoreleasepool
        {
//          ARA_LOG ("AUv3 IPC sends message %i%s", messageID, (_callbackLevel == 0) ? " starting new transaction" : " while handling message");
            NSDictionary * message { CFBridgingRelease (ARAIPCCFCopyMessageEncoderDictionaryAddingMessageID (encoder, messageID)) };
            _sendMessage (message);

            const auto previousAwaitsReply { _awaitsReply.load (std::memory_order_relaxed) };
            const auto previousReplyHandler { _replyHandler };
            const auto previousReplyHandlerUserData { _replyHandlerUserData };
            _replyHandler = replyHandler;
            _replyHandlerUserData = replyHandlerUserData;
            _awaitsReply.store (true, std::memory_order_release);
            do
            {
                if (_dequeueReceivedMessages () == 0)
                    dispatch_semaphore_wait (_receiveSemaphore, dispatch_time (DISPATCH_WALLTIME_NOW, 10*1000*1000));
            } while (_awaitsReply);
            _replyHandler = previousReplyHandler;
            _replyHandlerUserData = previousReplyHandlerUserData;
            _awaitsReply.store (previousAwaitsReply, std::memory_order_release);
//          ARA_LOG ("AUv3 IPC received reply to message %i%s", messageID, (_callbackLevel == 0) ? " ending transaction" : "");
        }
    }

    void enqueueReceivedMessage (NSDictionary * message, void * userData)
    {
        ARA_INTERNAL_ASSERT (![NSThread isMainThread]);

        _enqueueReceivedMessage (message, userData);

        if (!_awaitsReply.load (std::memory_order_acquire))
        {
            dispatch_async (dispatch_get_main_queue (),
            ^{
                int32_t ARA_MAYBE_UNUSED_VAR (count) { _dequeueReceivedMessages () };
                ARA_INTERNAL_ASSERT (count > 0);
            });
        }
    }

protected:
    virtual void _sendMessage (NSDictionary * message) = 0;
    virtual void _handleReceivedMessage (const ARAIPCMessageID messageID,
                                         const ARAIPCMessageDecoder * const decoder,
                                         void * const userData,
                                         ARAIPCMessageEncoder * const replyEncoder) = 0;

private:
    int32_t _dequeueReceivedMessages ()
    {
        int32_t count { 0 };
        @autoreleasepool
        {
            while (auto receivedMessage { _dequeueReceivedMessage () })
            {
                ++count;
                _processReceivedMessage ((__bridge CFDictionaryRef)receivedMessage->_message, receivedMessage->_userData);
                delete receivedMessage;
            }
        }
        return count;
    }

    void _processReceivedMessage (CFDictionaryRef message, void * userData)
    {
        auto messageDecoder { ARAIPCCFCreateMessageDecoderWithDictionary (message) };
        ARAIPCMessageID messageID { ARAIPCCFGetMessageIDFromDictionary (messageDecoder) };
        if (messageID != 0)
        {
            ++_callbackLevel;
//          ARA_LOG ("AUv3 IPC received message with ID %i%s", messageID, (_awaitsReply.load (std::memory_order_relaxed)) ? " while awaiting reply" : "");

            auto replyEncoder { createEncoder () };
            _handleReceivedMessage (messageID, messageDecoder, userData, replyEncoder);
            NSDictionary * reply { CFBridgingRelease (ARAIPCCFCopyMessageEncoderDictionaryAddingMessageID (replyEncoder, 0)) };
            delete replyEncoder;

//           ARA_LOG ("AUv3 IPC replies to message with ID %i", messageID);
            _sendMessage (reply);

            --_callbackLevel;
        }
        else
        {
            ARA_INTERNAL_ASSERT (_awaitsReply);
            if (_replyHandler)
            {
                auto replyDecoder { ARAIPCCFCreateMessageDecoderWithDictionary (message) };
                (*_replyHandler) (replyDecoder, _replyHandlerUserData);
                delete replyDecoder;
            }
            else
            {
                ARA_INTERNAL_ASSERT (CFDictionaryGetCount (message) == 1);   // reply should only contain message ID 0
            }
            _awaitsReply.store (false, std::memory_order_release);
        }
        delete messageDecoder;
    }

    void _enqueueReceivedMessage (NSDictionary * message, void * userData)
    {
        auto newMessage { new ReceivedMessage { message, userData } };
        auto nextMessage { _receivedMessage.load (std::memory_order_relaxed) };
        while (true)
        {
            newMessage->_nextMessage.store (nextMessage);
            if (_receivedMessage.compare_exchange_weak (nextMessage, newMessage, std::memory_order_release, std::memory_order_relaxed))
            {
                dispatch_semaphore_signal (_receiveSemaphore);
                break;
            }
        }
    }

    ReceivedMessage * _dequeueReceivedMessage ()
    {
        ARA_INTERNAL_ASSERT ([NSThread isMainThread]);

        auto current { &_receivedMessage };
        auto currentMessage { current->load (std::memory_order_consume) };
        if (!currentMessage)
            return nullptr;

        while (true)
        {
            auto next { &currentMessage->_nextMessage };
            auto nextMessage { next->load (std::memory_order_consume) };
            if (nextMessage == nullptr)
            {
                current->store (nullptr, std::memory_order_release);
                return currentMessage;
            }
            current = next;
            currentMessage = nextMessage;
        }
    }

protected:
    NSObject<AUMessageChannel> * __strong _messageChannel {};

private:
    dispatch_semaphore_t _receiveSemaphore;
    std::atomic<ReceivedMessage *> _receivedMessage {};    // simple multi-producer, single-consumer lockless queue
    int32_t _callbackLevel { 0 };
    std::atomic<bool> _awaitsReply {};
    ARAIPCReplyHandler * _replyHandler;
    void * _replyHandlerUserData;
};


class ARAIPCAUHostMessageSender : public ARAIPCAUMessageSender
{
public:
    using ARAIPCAUMessageSender::ARAIPCAUMessageSender;

protected:
    void _sendMessage (NSDictionary * message) override
    {
        ARA_INTERNAL_ASSERT ([NSThread isMainThread]);

        if (auto callHostBlock { _messageChannel.callHostBlock })
        {
            NSDictionary * reply { callHostBlock (message) };
            ARA_INTERNAL_ASSERT ([reply count] == 0);
        }
        else
        {
            ARA_INTERNAL_ASSERT (false && "trying to send IPC message while host has not set callHostBlock");
        }
    }

    void _handleReceivedMessage (const ARAIPCMessageID messageID,
                                 const ARAIPCMessageDecoder * const decoder,
                                 void * const userData,
                                 ARAIPCMessageEncoder * const replyEncoder) override
    {
        if (messageID == kARAIPCGetRemoteInstanceRef)
        {
            ARA_INTERNAL_ASSERT (userData != nullptr);
            replyEncoder->appendSize (0, (ARAIPCPlugInInstanceRef)userData);
        }
        else if (messageID == kARAIPCDestroyRemoteInstance)
        {
            ARA_INTERNAL_ASSERT (!decoder->isEmpty ());
            ARAIPCPlugInInstanceRef plugInInstanceRef;
            bool ARA_MAYBE_UNUSED_VAR (success) { decoder->readSize (0, &plugInInstanceRef) };
            ARA_INTERNAL_ASSERT (success);

            // \todo as long as we're using a separate channel per AUAudioUnit, we don't need to
            //       send the remote instance - it's known in the channel and provided as arg here
            ARA_INTERNAL_ASSERT ((void *)plugInInstanceRef == userData);
            _destructionHandler ((__bridge AUAudioUnit *)(void *)plugInInstanceRef);
        }
        else
        {
            ARAIPCProxyHostCommandHandler (messageID, decoder, replyEncoder);
        }
    }
};


class ARAIPCAUPlugInMessageSender : public ARAIPCAUMessageSender
{
public:
    ARAIPCAUPlugInMessageSender (NSObject<AUMessageChannel>* _Nonnull messageChannel)
    : ARAIPCAUMessageSender (messageChannel)
    {
        // \todo we happen to use the same message block for both the ARA_AUDIOUNIT_FACTORY_CUSTOM_MESSAGES_UTI
        //       and all ARA_AUDIOUNIT_PLUGINEXTENSION_CUSTOM_MESSAGES_UTI uses, but it is generally unclear
        //       how to set the correct message block if the channel is shared.
        _messageChannel.callHostBlock = ^NSDictionary * _Nullable (NSDictionary * _Nonnull message)
        {
            enqueueReceivedMessage (message, nullptr);
            return [NSDictionary dictionary];   // \todo it would yield better performance if the callHostBlock would allow nil as return value
        };
    }

    ~ARAIPCAUPlugInMessageSender () override
    {
        _messageChannel.callHostBlock = nil;
    }

protected:
    void _sendMessage (NSDictionary * message) override
    {
        ARA_INTERNAL_ASSERT ([NSThread isMainThread]);

        NSDictionary * reply { [_messageChannel callAudioUnit:message] };
        ARA_INTERNAL_ASSERT ([reply count] == 0);
    }

    void _handleReceivedMessage (const ARAIPCMessageID messageID,
                                 const ARAIPCMessageDecoder * const decoder,
                                 void * const /*userData*/,
                                 ARAIPCMessageEncoder * const replyEncoder) override
    {
        ARAIPCProxyPlugInCallbacksDispatcher (messageID, decoder, replyEncoder);
    }
};


class ARAIPCAUPlugInExtensionMessageSender : public ARAIPCAUPlugInMessageSender
{
    using ARAIPCAUPlugInMessageSender::ARAIPCAUPlugInMessageSender;

public:
    static ARAIPCAUPlugInExtensionMessageSender * bindAndCreateMessageSender (
                                                    NSObject<AUMessageChannel>* _Nonnull messageChannel,
                                                    ARADocumentControllerRef _Nonnull documentControllerRef,
                                                    ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles)
    {
        auto result { new ARAIPCAUPlugInExtensionMessageSender { messageChannel } };
        auto encoder { result->createEncoder () };
        ARAIPCReplyHandler replyHandler { [] (const ARAIPCMessageDecoder* decoder, void* userData) {
                ARA_INTERNAL_ASSERT (!decoder->isEmpty ());
                bool ARA_MAYBE_UNUSED_VAR (success) = decoder->readSize (0, (ARAIPCPlugInInstanceRef*)userData);
                ARA_INTERNAL_ASSERT (success);
            } };
        result->sendMessage (kARAIPCGetRemoteInstanceRef.getMessageID (), encoder, &replyHandler, &result->_remoteInstanceRef);
        delete encoder;

        result->_plugInExtensionInstance = ARAIPCProxyPlugInBindToDocumentController (result->_remoteInstanceRef, result, documentControllerRef, knownRoles, assignedRoles);

        return result;
    }

    ~ARAIPCAUPlugInExtensionMessageSender () override
    {
        if (_plugInExtensionInstance)
        {
            auto encoder { createEncoder () };
            encoder->appendSize (0, _remoteInstanceRef);
            sendMessage (kARAIPCDestroyRemoteInstance.getMessageID (), encoder, nullptr, nullptr);
            delete encoder;

            ARAIPCProxyPlugInCleanupBinding (_plugInExtensionInstance);
        }
    }

    const ARAPlugInExtensionInstance* getPlugInExtensionInstance () const
    {
        return _plugInExtensionInstance;
    }

private:
    ARAIPCPlugInInstanceRef _remoteInstanceRef {};                  // stores the remote AUAudioUnit<ARAAudioUnit> instance
    const ARAPlugInExtensionInstance* _plugInExtensionInstance {};
};



NSObject<AUMessageChannel>* _Nullable ARA_CALL ARAIPCAUGetMessageChannel (AUAudioUnit* _Nonnull audioUnit, NSString* _Nonnull identifier)
{
    // AUAudioUnits created before macOS 13 will not know about this API yet
    if (![audioUnit respondsToSelector:@selector(messageChannelFor:)])
        return nil;

    return (NSObject<AUMessageChannel>*)[(AUAudioUnit<ARAAudioUnit>*) audioUnit messageChannelFor:identifier];
}

ARAIPCMessageSender* _Nullable ARA_CALL ARAIPCAUProxyPlugInInitializeFactoryMessageSender (AUAudioUnit* _Nonnull audioUnit)
{
    auto messageChannel { ARAIPCAUGetMessageChannel (audioUnit, ARA_AUDIOUNIT_FACTORY_CUSTOM_MESSAGES_UTI) };
    if (!messageChannel)
        return nullptr;

    return new ARAIPCAUPlugInMessageSender { messageChannel };
}

const ARAFactory* _Nonnull ARA_CALL ARAIPCAUProxyPlugInGetFactory (ARAIPCMessageSender * _Nonnull messageSender)
{
    ARA_VALIDATE_API_CONDITION (ARAIPCProxyPlugInGetFactoriesCount (messageSender) == 1);
    const ARAFactory* result = ARAIPCProxyPlugInGetFactoryAtIndex (messageSender, 0);
    ARA_VALIDATE_API_CONDITION (result != nullptr);
    return result;
}

const ARAPlugInExtensionInstance* _Nullable ARA_CALL ARAIPCAUProxyPlugInBindToDocumentController (AUAudioUnit* _Nonnull audioUnit,
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

    *messageSender = ARAIPCAUPlugInExtensionMessageSender::bindAndCreateMessageSender (messageChannel,
                                                                                       documentControllerRef, knownRoles, assignedRoles);
    return static_cast<ARAIPCAUPlugInExtensionMessageSender*>(*messageSender)->getPlugInExtensionInstance ();
}

void ARA_CALL ARAIPCAUProxyPlugInCleanupBinding (ARAIPCMessageSender * messageSender)
{
    delete messageSender;
}

void ARA_CALL ARAIPCAUProxyPlugInUninitializeFactoryMessageSender (ARAIPCMessageSender* _Nonnull messageSender)
{
    delete messageSender;
}



ARAIPCAUHostMessageSender * _sharedPlugInCallbacksSender {};

void ARA_CALL ARAIPCAUProxyHostAddFactory (const ARAFactory* _Nonnull factory)
{
    ARAIPCProxyHostAddFactory (factory);
}

const ARAPlugInExtensionInstance* ARA_CALL ARAIPCAUBindingHandlerWrapper (ARAIPCPlugInInstanceRef plugInInstanceRef, ARADocumentControllerRef controllerRef,
                                                                          ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles)
{
    AUAudioUnit* audioUnit = (__bridge AUAudioUnit*)(void*)plugInInstanceRef;
    return _bindingHandler (audioUnit, controllerRef, knownRoles, assignedRoles);
}

void ARA_CALL ARAIPCAUProxyHostInitialize (NSObject<AUMessageChannel>* _Nonnull factoryMessageChannel, ARAIPCAUBindingHandler _Nonnull bindingHandler, ARAIPCAUDestructionHandler _Nonnull destructionHandler)
{
    _sharedPlugInCallbacksSender = new ARAIPCAUHostMessageSender (factoryMessageChannel);
    ARAIPCProxyHostSetPlugInCallbacksSender (_sharedPlugInCallbacksSender);

#if __has_feature(objc_arc)
    _bindingHandler = bindingHandler;
    _destructionHandler = destructionHandler;
#else
    _bindingHandler = [bindingHandler retain];
    _destructionHandler = [destructionHandler retain];
#endif
    ARAIPCProxyHostSetBindingHandler (ARAIPCAUBindingHandlerWrapper);
}

NSDictionary* _Nonnull ARA_CALL ARAIPCAUProxyHostCommandHandler (AUAudioUnit* _Nullable audioUnit, NSDictionary* _Nonnull message)
{
    _sharedPlugInCallbacksSender->enqueueReceivedMessage (message, audioUnit);
    return [NSDictionary dictionary];   // \todo it would yield better performance if the AUMessageChannel would accept nil as return value
}

void ARA_CALL ARAIPCAUProxyHostCleanupBinding (const ARAPlugInExtensionInstance* _Nonnull plugInExtensionInstance)
{
    ARAIPCProxyHostCleanupBinding (plugInExtensionInstance);
}

void ARA_CALL ARAIPCAUProxyHostUninitalize (void)
{
#if __has_feature(objc_arc)
    _bindingHandler = nil;
    _destructionHandler = nil;
#else
    [_bindingHandler release];
    [_destructionHandler release];
#endif
    delete _sharedPlugInCallbacksSender;
}



API_AVAILABLE_END


_Pragma ("GCC diagnostic pop")


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif


#endif // ARA_AUDIOUNITV3_IPC_IS_AVAILABLE
