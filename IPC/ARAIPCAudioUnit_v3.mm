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



class ARAIPCAUMessageSender : public ARAIPCMessageSender
{
public:
    ARAIPCAUMessageSender (NSObject<AUMessageChannel>* _Nonnull messageChannel, ARAIPCLockingContextRef _Nonnull lockingContextRef)
    : _messageChannel { messageChannel },
      _lockingContextRef { lockingContextRef }
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

protected:
    NSObject<AUMessageChannel>* __strong _messageChannel;
    ARAIPCLockingContextRef _lockingContextRef;
};


class ARAIPCAUHostMessageSender : public ARAIPCAUMessageSender
{
public:
    using ARAIPCAUMessageSender::ARAIPCAUMessageSender;

    void sendMessage (const bool stackable, ARAIPCMessageID messageID, ARAIPCMessageEncoder* encoder,
                      ARAIPCReplyHandler* replyHandler, void* replyHandlerUserData) override
    {
        @autoreleasepool
        {
            CallHostBlock callHostBlock = _messageChannel.callHostBlock;
            if (callHostBlock)
            {
                NSDictionary* message { CFBridgingRelease (ARAIPCCFCopyMessageEncoderDictionaryAddingMessageID (encoder, messageID)) };
                const auto lockToken { ARAIPCLockContextBeforeSendingMessage (_lockingContextRef, stackable) };
                if (replyHandler)
                {
                    auto reply { (__bridge CFDictionaryRef)callHostBlock (message) };
                    auto replyDecoder { ARAIPCCFCreateMessageDecoderWithDictionary (reply) };
                    (*replyHandler) (replyDecoder, replyHandlerUserData);
                    delete replyDecoder;
                }
                else
                {
                    auto reply { callHostBlock (message) };
                    ARA_INTERNAL_ASSERT ([reply count] == 0);
                }
                ARAIPCUnlockContextAfterSendingMessage (_lockingContextRef, lockToken);
            }
            else
            {
                ARA_INTERNAL_ASSERT (false && "trying to send IPC message while host has not set callHostBlock");
                if (replyHandler)
                {
                    auto replyDecoder { ARAIPCCFCreateMessageDecoderWithDictionary (nullptr) };
                    (*replyHandler) (replyDecoder, replyHandlerUserData);
                    delete replyDecoder;
                }
            }
        }
    }
};


class ARAIPCAUPlugInMessageSender : public ARAIPCAUMessageSender
{
public:
    ARAIPCAUPlugInMessageSender (NSObject<AUMessageChannel>* _Nonnull messageChannel, ARAIPCLockingContextRef _Nonnull lockingContextRef)
    : ARAIPCAUMessageSender (messageChannel, lockingContextRef)
    {
        // \todo we happen to use the same message block with the same locking context for both the
        //       ARA_AUDIOUNIT_FACTORY_CUSTOM_MESSAGES_UTI and all ARA_AUDIOUNIT_PLUGINEXTENSION_CUSTOM_MESSAGES_UTI uses,
        //       but it is generally unclear here how to set the correct message block if the channel is shared.
        _messageChannel.callHostBlock = ^NSDictionary* _Nullable (NSDictionary* _Nonnull message)
            {
                auto messageDecoder { ARAIPCCFCreateMessageDecoderWithDictionary ((__bridge CFDictionaryRef)message) };
                ARAIPCMessageID messageID = ARAIPCCFGetMessageIDFromDictionary (messageDecoder);

                auto replyEncoder { ARAIPCCFCreateMessageEncoder () };

                const ARAIPCLockingContextMessageHandlingToken lockToken { ARAIPCLockContextBeforeHandlingMessage (lockingContextRef) };
                ARAIPCProxyPlugInCallbacksDispatcher (messageID, messageDecoder, replyEncoder);
                ARAIPCUnlockContextAfterHandlingMessage (lockingContextRef, lockToken);

                delete messageDecoder;

                NSDictionary* replyDictionary = CFBridgingRelease (ARAIPCCFCopyMessageEncoderDictionary (replyEncoder));
                delete replyEncoder;
                return replyDictionary;
            };
    }

    ~ARAIPCAUPlugInMessageSender () override
    {
        _messageChannel.callHostBlock = nil;
    }

    void sendMessage (const bool stackable, ARAIPCMessageID messageID, ARAIPCMessageEncoder* encoder,
                      ARAIPCReplyHandler* replyHandler, void* replyHandlerUserData) override
    {
        @autoreleasepool
        {
            NSDictionary* message { CFBridgingRelease (ARAIPCCFCopyMessageEncoderDictionaryAddingMessageID (encoder, messageID)) };
            const auto lockToken { ARAIPCLockContextBeforeSendingMessage (_lockingContextRef, stackable) };
            if (replyHandler)
            {
                auto reply { (__bridge CFDictionaryRef)[_messageChannel callAudioUnit:message] };
                auto replyDecoder { ARAIPCCFCreateMessageDecoderWithDictionary (reply) };
                (*replyHandler) (replyDecoder, replyHandlerUserData);
                delete replyDecoder;
            }
            else
            {
                auto reply { [_messageChannel callAudioUnit:message] };
                ARA_INTERNAL_ASSERT ([reply count] == 0);
            }
            ARAIPCUnlockContextAfterSendingMessage (_lockingContextRef, lockToken);
        }
    }
};


class ARAIPCAUPlugInExtensionMessageSender : public ARAIPCAUPlugInMessageSender
{
    using ARAIPCAUPlugInMessageSender::ARAIPCAUPlugInMessageSender;

public:
    static ARAIPCAUPlugInExtensionMessageSender * bindAndCreateMessageSender (
                                                    NSObject<AUMessageChannel>* _Nonnull messageChannel, ARAIPCLockingContextRef _Nonnull lockingContextRef,
                                                    ARADocumentControllerRef _Nonnull documentControllerRef,
                                                    ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles)
    {
        auto result { new ARAIPCAUPlugInExtensionMessageSender { messageChannel, lockingContextRef } };
        auto encoder { result->createEncoder () };
        ARAIPCReplyHandler replyHandler { [] (const ARAIPCMessageDecoder* decoder, void* userData) {
                ARA_INTERNAL_ASSERT (!decoder->isEmpty ());
                bool ARA_MAYBE_UNUSED_VAR (success) = decoder->readSize (0, (ARAIPCPlugInInstanceRef*)userData);
                ARA_INTERNAL_ASSERT (success);
            } };
        result->sendMessage (false, kARAIPCGetRemoteInstanceRef.getMessageID (), encoder, &replyHandler, &result->_remoteInstanceRef);
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
            sendMessage (false, kARAIPCDestroyRemoteInstance.getMessageID (), encoder, nullptr, nullptr);
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

ARAIPCMessageSender* _Nullable ARA_CALL ARAIPCAUProxyPlugInInitializeFactoryMessageSender (AUAudioUnit* _Nonnull audioUnit,
                                                                                           ARAIPCLockingContextRef _Nonnull lockingContextRef)
{
    auto messageChannel { ARAIPCAUGetMessageChannel (audioUnit, ARA_AUDIOUNIT_FACTORY_CUSTOM_MESSAGES_UTI) };
    if (!messageChannel)
        return nullptr;

    return new ARAIPCAUPlugInMessageSender { messageChannel, lockingContextRef };
}

const ARAFactory* _Nonnull ARA_CALL ARAIPCAUProxyPlugInGetFactory (ARAIPCMessageSender * _Nonnull messageSender)
{
    ARA_VALIDATE_API_CONDITION (ARAIPCProxyPlugInGetFactoriesCount (messageSender) == 1);
    const ARAFactory* result = ARAIPCProxyPlugInGetFactoryAtIndex (messageSender, 0);
    ARA_VALIDATE_API_CONDITION (result != nullptr);
    return result;
}

const ARAPlugInExtensionInstance* _Nullable ARA_CALL ARAIPCAUProxyPlugInBindToDocumentController (AUAudioUnit* _Nonnull audioUnit,
                                                                                                  ARAIPCLockingContextRef _Nonnull lockingContextRef,
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

    *messageSender = ARAIPCAUPlugInExtensionMessageSender::bindAndCreateMessageSender (messageChannel, lockingContextRef,
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



ARAIPCLockingContextRef _sharedPlugInLockingContextRef {};
ARAIPCMessageSender* _sharedPlugInCallbacksSender {};

void ARA_CALL ARAIPCAUProxyHostAddFactory (const ARAFactory* _Nonnull factory)
{
    ARAIPCProxyHostAddFactory (factory);
}

ARAIPCAUBindingHandler _bindingHandler = nil;
ARAIPCAUDestructionHandler _destructionHandler = nil;

const ARAPlugInExtensionInstance* ARA_CALL ARAIPCAUBindingHandlerWrapper (ARAIPCPlugInInstanceRef plugInInstanceRef, ARADocumentControllerRef controllerRef,
                                                                          ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles)
{
    AUAudioUnit* audioUnit = (__bridge AUAudioUnit*)(void*)plugInInstanceRef;
    return _bindingHandler (audioUnit, controllerRef, knownRoles, assignedRoles);
}

void ARA_CALL ARAIPCAUProxyHostInitialize (NSObject<AUMessageChannel>* _Nonnull factoryMessageChannel, ARAIPCAUBindingHandler _Nonnull bindingHandler, ARAIPCAUDestructionHandler _Nonnull destructionHandler)
{
    _sharedPlugInLockingContextRef = ARAIPCCreateLockingContext ();

    _sharedPlugInCallbacksSender = new ARAIPCAUHostMessageSender (factoryMessageChannel, _sharedPlugInLockingContextRef);
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
    auto messageDecoder { ARAIPCCFCreateMessageDecoderWithDictionary ((__bridge CFDictionaryRef)message) };
    const auto messageID { ARAIPCCFGetMessageIDFromDictionary (messageDecoder) };

    auto replyEncoder { ARAIPCCFCreateMessageEncoder () };

    if (messageID == kARAIPCGetRemoteInstanceRef)
    {
        replyEncoder->appendSize (0, (ARAIPCPlugInInstanceRef)audioUnit);
    }
    else if (messageID == kARAIPCDestroyRemoteInstance)
    {
        ARA_INTERNAL_ASSERT (!messageDecoder->isEmpty ());
        ARAIPCPlugInInstanceRef plugInInstanceRef;
        bool ARA_MAYBE_UNUSED_VAR (success) = messageDecoder->readSize (0, &plugInInstanceRef);
        ARA_INTERNAL_ASSERT (success);

        // \todo as long as we're using a separate channel per AUAudioUnit, we don't need to
        //       send the remote instance - it's known in the channel and provided as arg here
        ARA_INTERNAL_ASSERT ((__bridge AUAudioUnit*)(void*)plugInInstanceRef == audioUnit);
        _destructionHandler ((__bridge AUAudioUnit*)(void*)plugInInstanceRef);
    }
    else
    {
        const auto lockToken { ARAIPCLockContextBeforeHandlingMessage (_sharedPlugInLockingContextRef) };
        ARAIPCProxyHostCommandHandler (messageID, messageDecoder, replyEncoder);
        ARAIPCUnlockContextAfterHandlingMessage (_sharedPlugInLockingContextRef, lockToken);
    }

    delete messageDecoder;

    NSDictionary* replyDictionary { CFBridgingRelease (ARAIPCCFCopyMessageEncoderDictionary (replyEncoder)) };
    delete replyEncoder;
    return replyDictionary;
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
    ARAIPCDestroyLockingContext (_sharedPlugInLockingContextRef);
}



API_AVAILABLE_END


_Pragma ("GCC diagnostic pop")


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif


#endif // ARA_AUDIOUNITV3_IPC_IS_AVAILABLE
