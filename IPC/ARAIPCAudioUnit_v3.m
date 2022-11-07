//------------------------------------------------------------------------------
//! \file       ARAIPCAudioUnit_v3.m
//!             Implementation of ARA IPC message sending through AUMessageChannel
//! \project    ARA SDK Examples
//! \copyright  Copyright (c) 2021-2022, Celemony Software GmbH, All Rights Reserved.
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


API_AVAILABLE_BEGIN(macos(13.0))


// custom IPC message to read the remote instance ref
const ARAIPCMessageID kARAIPCGetRemoteInstanceRef = -1;


// "base class" layout used in plug-in and host
typedef struct ARAIPCMessageSenderImplementation
{
    NSObject<AUMessageChannel> * __strong messageChannel;
    ARAIPCLockingContextRef lockingContextRef;
} ARAIPCMessageSenderImplementation;

// "subclass" used for plug-in extension proxies with added fields
typedef struct ARAIPCAUProxyPlugInExtensionMessageSenderImplementation
{
    ARAIPCMessageSenderImplementation base;
    ARAIPCPlugInInstanceRef remoteInstanceRef;                  // stores "ref" for the remote ARAIPCAUAudioUnit<ARAAudioUnit> instance
    const ARAPlugInExtensionInstance * plugInExtensionInstance; // stored only for proper cleanup
} ARAIPCAUProxyPlugInExtensionMessageSenderImplementation;


// message sender implementations for host and plug-in
ARAIPCMessageEncoder ARA_CALL ARAIPCAUCreateEncoder(ARAIPCMessageSenderRef ARA_MAYBE_UNUSED_ARG(messageSenderRef))
{
    return ARAIPCCFCreateMessageEncoder();
}

void ARA_CALL ARAIPCAUProxyPlugInSendMessage(const bool stackable, ARAIPCMessageSenderRef messageSenderRef, ARAIPCMessageID messageID, const ARAIPCMessageEncoder * encoder,
                                                ARAIPCReplyHandler * const replyHandler, void * replyHandlerUserData)
{
    @autoreleasepool
    {
        NSDictionary * message = CFBridgingRelease(ARAIPCCFCopyMessageEncoderDictionaryAddingMessageID(encoder->ref, messageID));
        const ARAIPCLockingContextMessageSendingToken lockToken = ARAIPCLockContextBeforeSendingMessage(messageSenderRef->lockingContextRef, stackable);
        if (replyHandler)
        {
            CFDictionaryRef _Nonnull reply = (__bridge CFDictionaryRef)[messageSenderRef->messageChannel callAudioUnit:message];
            ARAIPCMessageDecoder replyDecoder = ARAIPCCFCreateMessageDecoderWithDictionary(reply);
            (*replyHandler)(replyDecoder, replyHandlerUserData);
            replyDecoder.methods->destroyDecoder(replyDecoder.ref);
        }
        else
        {
            NSDictionary * _Nonnull reply = [messageSenderRef->messageChannel callAudioUnit:message];
            ARA_INTERNAL_ASSERT([reply count] == 0);
        }
        ARAIPCUnlockContextAfterSendingMessage(messageSenderRef->lockingContextRef, lockToken);
    }
}

void ARA_CALL ARAIPCAUProxyHostSendMessage(const bool stackable, ARAIPCMessageSenderRef messageSenderRef, ARAIPCMessageID messageID, const ARAIPCMessageEncoder * encoder,
                                            ARAIPCReplyHandler * const replyHandler, void * replyHandlerUserData)
{
    @autoreleasepool
    {
        CallHostBlock callHostBlock = messageSenderRef->messageChannel.callHostBlock;
        if (callHostBlock)
        {
            NSDictionary * message = CFBridgingRelease(ARAIPCCFCopyMessageEncoderDictionaryAddingMessageID(encoder->ref, messageID));
            const ARAIPCLockingContextMessageSendingToken lockToken = ARAIPCLockContextBeforeSendingMessage(messageSenderRef->lockingContextRef, stackable);
            if (replyHandler)
            {
                CFDictionaryRef _Nullable reply = (__bridge CFDictionaryRef)callHostBlock(message);
                ARAIPCMessageDecoder replyDecoder = ARAIPCCFCreateMessageDecoderWithDictionary(reply);
                (*replyHandler)(replyDecoder, replyHandlerUserData);
                replyDecoder.methods->destroyDecoder(replyDecoder.ref);
            }
            else
            {
                NSDictionary * _Nullable reply = callHostBlock(message);
                ARA_INTERNAL_ASSERT([reply count] == 0);
            }
            ARAIPCUnlockContextAfterSendingMessage(messageSenderRef->lockingContextRef, lockToken);
        }
        else
        {
            ARA_INTERNAL_ASSERT(false && "trying to send IPC message while host has not set callHostBlock");
            if (replyHandler)
            {
                ARAIPCMessageDecoder replyDecoder = ARAIPCCFCreateMessageDecoderWithDictionary(NULL);
                (*replyHandler)(replyDecoder, replyHandlerUserData);
                replyDecoder.methods->destroyDecoder(replyDecoder.ref);
            }
        }
    }
}

bool ARA_CALL ARAIPCAUReceiverEndianessMatches(ARAIPCMessageSenderRef ARA_MAYBE_UNUSED_ARG(messageSenderRef))
{
    // \todo shouldn't the AUMessageChannel provide this information?
    return true;
}

ARAIPCMessageSender ARA_CALL ARAIPCAUInitializeSender(ARAIPCMessageSenderImplementation * ref, const ARAIPCMessageSenderInterface * methods,
                                                      NSObject<AUMessageChannel> * _Nonnull messageChannel, ARAIPCLockingContextRef _Nonnull lockingContextRef)
{
    ARAIPCMessageSender sender;
    sender.methods = methods;

    sender.ref = ref;
    ARA_INTERNAL_ASSERT(sender.ref != NULL);
#if __has_feature(objc_arc)
    memset((void*)ref, 0, sizeof(((ARAIPCMessageSenderImplementation *)NULL)->messageChannel)); // partially clear malloced memory or else ARC will try to release some "old object" upon assign
    sender.ref->messageChannel = messageChannel;
#else
    sender.ref->messageChannel = [messageChannel retain];
#endif
    sender.ref->lockingContextRef = lockingContextRef;

    return sender;
}

void ARA_CALL ARAIPCAUDestroyMessageSender(ARAIPCMessageSender messageSender)
{
#if __has_feature(objc_arc)
    messageSender.ref->messageChannel = nil;
#else
    [messageSender.ref->messageChannel release];
#endif
    free(messageSender.ref);
}



void ARA_CALL ARAIPCAUGetRemoteInstanceReplyHandler(const ARAIPCMessageDecoder decoder, void * _Nullable userData)
{
    ARA_INTERNAL_ASSERT(!decoder.methods->isEmpty(decoder.ref));
    bool ARA_MAYBE_UNUSED_VAR(success) = decoder.methods->readSize(decoder.ref, 0, (ARAIPCPlugInInstanceRef*)userData);
    ARA_INTERNAL_ASSERT(success);
}

ARAIPCMessageSender ARA_CALL ARAIPCAUProxyPlugInCreateMessageSender(NSObject<AUMessageChannel> * _Nonnull messageChannel,
                                                                    ARAIPCMessageSenderRef messageSenderRef,
                                                                    ARAIPCLockingContextRef _Nonnull lockingContextRef)
{
    static const ARAIPCMessageSenderInterface senderInterface = { ARAIPCAUCreateEncoder, ARAIPCAUProxyPlugInSendMessage, ARAIPCAUReceiverEndianessMatches };
    ARAIPCMessageSender sender = ARAIPCAUInitializeSender(messageSenderRef, &senderInterface, messageChannel, lockingContextRef);

    // \todo we happen to use the same message block with the same locking context for both the
    //       ARA_AUDIOUNIT_FACTORY_CUSTOM_MESSAGES_UTI and all ARA_AUDIOUNIT_PLUGINEXTENSION_CUSTOM_MESSAGES_UTI uses,
    //       but it is generally unclear here how to set the correct message block if the channel is shared.
    messageChannel.callHostBlock = ^NSDictionary * _Nullable (NSDictionary * _Nonnull message)
        {
            ARAIPCMessageDecoder messageDecoder = ARAIPCCFCreateMessageDecoderWithDictionary((__bridge CFDictionaryRef)message);
            ARAIPCMessageID messageID = ARAIPCCFGetMessageIDFromDictionary(messageDecoder.ref);

            ARAIPCMessageEncoder replyEncoder = ARAIPCCFCreateMessageEncoder();

            const ARAIPCLockingContextMessageHandlingToken lockToken = ARAIPCLockContextBeforeHandlingMessage(lockingContextRef);
            ARAIPCProxyPlugInCallbacksDispatcher(messageID, &messageDecoder, &replyEncoder);
            ARAIPCUnlockContextAfterHandlingMessage(lockingContextRef, lockToken);

            messageDecoder.methods->destroyDecoder(messageDecoder.ref);

            NSDictionary * replyDictionary = CFBridgingRelease(ARAIPCCFCopyMessageEncoderDictionary(replyEncoder.ref));
            replyEncoder.methods->destroyEncoder(replyEncoder.ref);
            return replyDictionary;
        };

    return sender;
}

id<AUMessageChannel> _Nullable ARA_CALL ARAIPCAUGetMessageChannel(AUAudioUnit * _Nonnull audioUnit, NSString * _Nonnull identifier)
{
    // AUAudioUnits created before macOS 13 will not know about this API yet
    if (![audioUnit respondsToSelector:@selector(messageChannelFor:)])
        return nil;

    return [(AUAudioUnit<ARAAudioUnit> *) audioUnit messageChannelFor:identifier];
}

bool ARA_CALL ARAIPCAUProxyPlugInInitializeFactoryMessageSender(ARAIPCMessageSender * _Nonnull messageSender,
                                                                AUAudioUnit * _Nonnull audioUnit, ARAIPCLockingContextRef _Nonnull lockingContextRef)
{
    id<AUMessageChannel> _Nullable messageChannel = ARAIPCAUGetMessageChannel(audioUnit, ARA_AUDIOUNIT_FACTORY_CUSTOM_MESSAGES_UTI);
    if (!messageChannel)
        return false;

    ARAIPCMessageSenderImplementation * messageSenderRef = malloc(sizeof(ARAIPCMessageSenderImplementation));
    *messageSender = ARAIPCAUProxyPlugInCreateMessageSender((NSObject<AUMessageChannel> * _Nonnull)messageChannel, messageSenderRef, lockingContextRef);
    return true;
}

const ARAFactory * _Nonnull ARA_CALL ARAIPCAUProxyPlugInGetFactory(ARAIPCMessageSender messageSender)
{
    ARA_VALIDATE_API_CONDITION(ARAIPCProxyPlugInGetFactoriesCount(messageSender) == 1);
    const ARAFactory * result = ARAIPCProxyPlugInGetFactoryAtIndex(messageSender, 0);
    ARA_VALIDATE_API_CONDITION(result != NULL);
    return result;
}

const ARAPlugInExtensionInstance * _Nullable ARA_CALL ARAIPCAUProxyPlugInBindToDocumentController(AUAudioUnit * _Nonnull audioUnit,
                                                                                                    ARAIPCLockingContextRef _Nonnull lockingContextRef,
                                                                                                    ARADocumentControllerRef _Nonnull documentControllerRef,
                                                                                                    ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles,
                                                                                                    ARAIPCMessageSender * _Nonnull messageSender)
{
    id<AUMessageChannel> _Nullable messageChannel = ARAIPCAUGetMessageChannel(audioUnit, ARA_AUDIOUNIT_PLUGINEXTENSION_CUSTOM_MESSAGES_UTI);
    if (!messageChannel)
        return NULL;

    ARAIPCAUProxyPlugInExtensionMessageSenderImplementation * messageSenderRef = malloc(sizeof(ARAIPCAUProxyPlugInExtensionMessageSenderImplementation));
    messageSenderRef->plugInExtensionInstance = NULL;
    *messageSender = ARAIPCAUProxyPlugInCreateMessageSender((NSObject<AUMessageChannel> * _Nonnull)messageChannel, (ARAIPCMessageSenderImplementation *)messageSenderRef, lockingContextRef);

    ARAIPCMessageEncoder encoder = messageSender->methods->createEncoder(messageSender->ref);
    ARAIPCReplyHandler replyHandler = ARAIPCAUGetRemoteInstanceReplyHandler;
    messageSender->methods->sendMessage(false, messageSender->ref, kARAIPCGetRemoteInstanceRef, &encoder, &replyHandler, &messageSenderRef->remoteInstanceRef);
    encoder.methods->destroyEncoder(encoder.ref);

    messageSenderRef->plugInExtensionInstance = ARAIPCProxyPlugInBindToDocumentController(messageSenderRef->remoteInstanceRef, *messageSender, documentControllerRef, knownRoles, assignedRoles);
    return messageSenderRef->plugInExtensionInstance;
}

void ARA_CALL ARAIPCAUProxyPlugInCleanupBinding(ARAIPCMessageSender messageSender)
{
    ARAIPCAUProxyPlugInExtensionMessageSenderImplementation * messageSenderRef = (ARAIPCAUProxyPlugInExtensionMessageSenderImplementation *)messageSender.ref;
    if (messageSenderRef->plugInExtensionInstance)
        ARAIPCProxyPlugInCleanupBinding(messageSenderRef->plugInExtensionInstance);

    ARAIPCAUDestroyMessageSender(messageSender);
}

void ARA_CALL ARAIPCAUProxyPlugInUninitializeFactoryMessageSender(ARAIPCMessageSender messageSender)
{
    ARAIPCAUDestroyMessageSender(messageSender);
}



ARAIPCLockingContextRef _sharedPlugInLockingContextRef;
ARAIPCMessageSender _sharedPlugInCallbacksSender;

void ARA_CALL ARAIPCAUProxyHostAddFactory(const ARAFactory * _Nonnull factory)
{
    ARAIPCProxyHostAddFactory(factory);
}

ARAIPCAUBindingHandler _bindingHandler = nil;

const ARAPlugInExtensionInstance * ARA_CALL ARAIPCAUBindingHandlerWrapper(ARAIPCPlugInInstanceRef plugInInstanceRef, ARADocumentControllerRef controllerRef,
                                                                          ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles)
{
    AUAudioUnit * audioUnit = (__bridge AUAudioUnit *)(void *)plugInInstanceRef;
    return _bindingHandler (audioUnit, controllerRef, knownRoles, assignedRoles);
}

void ARA_CALL ARAIPCAUProxyHostInitialize(NSObject<AUMessageChannel> * _Nonnull factoryMessageChannel, ARAIPCAUBindingHandler _Nonnull bindingHandler)
{
    _sharedPlugInLockingContextRef = ARAIPCCreateLockingContext();

    static const ARAIPCMessageSenderInterface senderInterface = { ARAIPCAUCreateEncoder, ARAIPCAUProxyHostSendMessage, ARAIPCAUReceiverEndianessMatches };
    _sharedPlugInCallbacksSender = ARAIPCAUInitializeSender(malloc(sizeof(ARAIPCMessageSenderImplementation)), &senderInterface, factoryMessageChannel, _sharedPlugInLockingContextRef);
    ARAIPCProxyHostSetPlugInCallbacksSender(_sharedPlugInCallbacksSender);

#if __has_feature(objc_arc)
    _bindingHandler = bindingHandler;
#else
    _bindingHandler = [bindingHandler retain];
#endif
    ARAIPCProxyHostSetBindingHandler(ARAIPCAUBindingHandlerWrapper);
}

NSDictionary * _Nonnull ARA_CALL ARAIPCAUProxyHostCommandHandler (AUAudioUnit * _Nullable audioUnit, NSDictionary * _Nonnull message)
{
    ARAIPCMessageDecoder messageDecoder = ARAIPCCFCreateMessageDecoderWithDictionary((__bridge CFDictionaryRef)message);
    ARAIPCMessageID messageID = ARAIPCCFGetMessageIDFromDictionary(messageDecoder.ref);

    ARAIPCMessageEncoder replyEncoder = ARAIPCCFCreateMessageEncoder();

    if (messageID == kARAIPCGetRemoteInstanceRef)
    {
        replyEncoder.methods->appendSize(replyEncoder.ref, 0, (ARAIPCPlugInInstanceRef)audioUnit);
    }
    else
    {
        const ARAIPCLockingContextMessageHandlingToken lockToken = ARAIPCLockContextBeforeHandlingMessage(_sharedPlugInLockingContextRef);
        ARAIPCProxyHostCommandHandler(messageID, &messageDecoder, &replyEncoder);
        ARAIPCUnlockContextAfterHandlingMessage(_sharedPlugInLockingContextRef, lockToken);
    }

    messageDecoder.methods->destroyDecoder(messageDecoder.ref);

    NSDictionary * replyDictionary = CFBridgingRelease(ARAIPCCFCopyMessageEncoderDictionary(replyEncoder.ref));
    replyEncoder.methods->destroyEncoder(replyEncoder.ref);
    return replyDictionary;
}

void ARA_CALL ARAIPCAUProxyHostCleanupBinding(const ARAPlugInExtensionInstance * _Nonnull plugInExtensionInstance)
{
    ARAIPCProxyHostCleanupBinding(plugInExtensionInstance);
}

void ARA_CALL ARAIPCAUProxyHostUninitalize(void)
{
#if __has_feature(objc_arc)
    _bindingHandler = nil;
#else
    [_bindingHandler release];
#endif
    ARAIPCAUDestroyMessageSender(_sharedPlugInCallbacksSender);
    ARAIPCDestroyLockingContext(_sharedPlugInLockingContextRef);
}

    
API_AVAILABLE_END


#if defined(__cplusplus)
}   // extern "C"
}   // namespace IPC
}   // namespace ARA
#endif


#endif // ARA_AUDIOUNITV3_IPC_IS_AVAILABLE
