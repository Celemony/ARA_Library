//------------------------------------------------------------------------------
//! \file       ARAIPCAudioUnit_v3.m
//!             Implementation of ARA IPC message sending through AUMessageChannel
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

#import "ARAIPCAudioUnit_v3.h"


#if ARA_AUDIOUNITV3_IPC_IS_AVAILABLE


#import "ARA_Library/Debug/ARADebug.h"
#import "ARA_Library/IPC/ARAIPCEncoding.h"
#import "ARA_Library/IPC/ARAIPCCFEncoding.h"
#import "ARA_Library/IPC/ARAIPCMessageChannel.h"
#import "ARA_Library/IPC/ARAIPCProxyHost.h"
#import "ARA_Library/IPC/ARAIPCProxyPlugIn.h"
#if !ARA_ENABLE_IPC
    #error "configuration mismatch: enabling ARA_AUDIOUNITV3_IPC_IS_AVAILABLE requires enabling ARA_ENABLE_IPC too"
#endif

#include <atomic>


namespace ARA {
namespace IPC {


_Pragma ("GCC diagnostic push")
_Pragma ("GCC diagnostic ignored \"-Wold-style-cast\"") // __bridge casts can only be done old-style


API_AVAILABLE_BEGIN(macos(13.0))


// key for transaction locking through the IPC channel
constexpr NSString * _messageIDKey { @"msgID" };


// message channel base class for both proxy implementations
class AudioUnitMessageChannel : public MessageChannel
{
protected:
    AudioUnitMessageChannel (MessageHandler * messageHandler, NSObject<AUMessageChannel> * _Nonnull audioUnitChannel)
    : MessageChannel { messageHandler },
      _audioUnitChannel { audioUnitChannel }
    {
#if !__has_feature(objc_arc)
        [_audioUnitChannel retain];
#endif
    }

public:
#if !__has_feature(objc_arc)
    ~AudioUnitMessageChannel () override
    {
        [_audioUnitChannel release];
    }
#endif

    MessageEncoder * createEncoder () override
    {
        return new CFMessageEncoder {};
    }

    bool receiverEndianessMatches () override
    {
        // \todo shouldn't the AUMessageChannel provide this information?
        return true;
    }

    void routeReceivedMessage (NSDictionary * _Nonnull message)
    {
        const MessageID messageID { [(NSNumber *) [message objectForKey:_messageIDKey] intValue] };
        const auto decoder { new CFMessageDecoder { (__bridge CFDictionaryRef) message } };
        MessageChannel::routeReceivedMessage (messageID, decoder);
    }

    void _sendMessage (MessageID messageID, MessageEncoder * encoder) override
    {
        const auto dictionary { static_cast<CFMessageEncoder *> (encoder)->copyDictionary () };
#if !__has_feature(objc_arc)
        auto message { (__bridge NSMutableDictionary *) dictionary };
#else
        auto message { (__bridge_transfer NSMutableDictionary *) dictionary };
#endif
        [message setObject:[NSNumber numberWithInt: messageID] forKey:_messageIDKey];
        const auto reply { _sendMessage (message) };
        ARA_INTERNAL_ASSERT ([reply count] == 0);
#if !__has_feature(objc_arc)
        CFRelease (dictionary);
#endif
    }

protected:
    virtual NSDictionary * _sendMessage (NSDictionary * message) = 0;

protected:
    NSObject<AUMessageChannel> * __strong _Nonnull _audioUnitChannel;
};


// plug-in side: proxy host message channel specialization
class ProxyHostMessageChannel : public AudioUnitMessageChannel, public ProxyHostMessageHandler
{
public:
    ProxyHostMessageChannel (NSObject<AUMessageChannel> * _Nonnull audioUnitChannel)
    : AudioUnitMessageChannel { this, audioUnitChannel }
    {}

protected:
    NSDictionary * _sendMessage (NSDictionary * message) override
    {
        if (const auto callHostBlock { _audioUnitChannel.callHostBlock })
        {
            NSDictionary * reply { callHostBlock (message) };
            return reply;
        }
        else
        {
            ARA_INTERNAL_ASSERT (false && "trying to send IPC message while host has not set callHostBlock");
            return nil;
        }
    }
};


// host side: proxy plug-in message channel specialization
class ProxyPlugInMessageChannel : public AudioUnitMessageChannel, public ProxyPlugInMessageHandler
{
public:
    ProxyPlugInMessageChannel (NSObject<AUMessageChannel> * _Nonnull audioUnitChannel)
    : AudioUnitMessageChannel { this, audioUnitChannel }
    {
        // \todo there's also QOS_CLASS_USER_INTERACTIVE which seems more appropriate but is undocumented...
#if __has_feature(objc_arc)
        if (!_readAudioQueue)
#else
        if (_instanceCount == 0)
#endif
            _readAudioQueue = dispatch_queue_create ("ARA read audio samples", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, -1));
#if !__has_feature(objc_arc)
        ++_instanceCount;
#endif
        _audioUnitChannel.callHostBlock =
            ^NSDictionary * _Nullable (NSDictionary * _Nonnull message)
            {
                routeReceivedMessage (message);
                return [NSDictionary dictionary];   // \todo it would yield better performance if the callHostBlock would allow nil as return value
            };
    }

    ~ProxyPlugInMessageChannel () override
    {
        _audioUnitChannel.callHostBlock = nil;
#if !__has_feature(objc_arc)
        --_instanceCount;
        if (_instanceCount == 0)
            dispatch_release (_readAudioQueue);
#endif
    }

    DispatchTarget getDispatchTargetForIncomingTransaction (MessageID messageID) override
    {
        // AUMessageChannel cannot be called back from the same thread it receives the message,
        // so we dispatch to the main queue for playback requests and to a dedicated read samples queue for audio requests
        // \todo maybe we should make this configurable, so hosts can set these queues if they already have appropriate ones?
        if (messageID == ARA_IPC_HOST_METHOD_ID (ARAAudioAccessControllerInterface, readAudioSamples).getMessageID ())
        {
            return _readAudioQueue;
        }
        else
        {
            ARA_INTERNAL_ASSERT ((ARA_IPC_HOST_METHOD_ID (ARAPlaybackControllerInterface, requestStartPlayback).getMessageID () <= messageID) &&
                                 (messageID <= ARA_IPC_HOST_METHOD_ID (ARAPlaybackControllerInterface, requestEnableCycle).getMessageID ()));
            return dispatch_get_main_queue ();
        }
    }

protected:
    NSDictionary * _sendMessage (NSDictionary * message) override
    {
        const auto reply { [_audioUnitChannel callAudioUnit:message] };
        return reply;
    }

private:
    static dispatch_queue_t _readAudioQueue;
#if !__has_feature(objc_arc)
    static int _instanceCount;
#endif
};

dispatch_queue_t ProxyPlugInMessageChannel::_readAudioQueue { nullptr };
#if !__has_feature(objc_arc)
int ProxyPlugInMessageChannel::_instanceCount { 0 };
#endif


struct ProxyPlugInComponent
{
    ARAIPCMessageChannelRef mainChannelRef;
};


#if defined (__GNUC__)
    _Pragma ("GCC diagnostic push")
    _Pragma ("GCC diagnostic ignored \"-Wunguarded-availability\"")
#endif

ARA_MAP_IPC_REF (AudioUnitMessageChannel, ARA::IPC::ARAIPCMessageChannelRef)
ARA_MAP_IPC_REF (ProxyPlugInComponent, ARAIPCConnectionRef)

#if defined (__GNUC__)
    _Pragma ("GCC diagnostic pop")
#endif


extern "C" {


// host side: proxy plug-in C adapter
ProxyPlugInMessageChannel* _Nullable _ARAIPCAUCreateMessageChannel (AUAudioUnit * _Nonnull audioUnit, NSString * _Nonnull identifier)
{
    id<AUMessageChannel> channel { nil };
    // AUAudioUnits created before macOS 13 will not know about this API yet
    if ([audioUnit respondsToSelector:@selector(messageChannelFor:)])
        channel = [audioUnit messageChannelFor:identifier];

    if (!channel)
        return nullptr;

    return new ProxyPlugInMessageChannel { (NSObject<AUMessageChannel> * _Nonnull) channel };
}

ARAIPCConnectionRef ARA_CALL ARAIPCAUProxyPlugInInitialize (AUAudioUnit * _Nonnull audioUnit)
{
    auto mainThreadChannel { _ARAIPCAUCreateMessageChannel (audioUnit, ARA_AUDIOUNIT_FACTORY_CUSTOM_MESSAGES_UTI) };
    if (!mainThreadChannel)
        return nullptr;

    return toIPCRef (new ProxyPlugInComponent { toIPCRef (mainThreadChannel) });
}

ARAIPCMessageChannelRef _Nonnull ARA_CALL ARAIPCAUProxyPlugInGetMainMessageChannel(ARAIPCConnectionRef _Nonnull proxyRef)
{
    return fromIPCRef (proxyRef)->mainChannelRef;
}

const ARAPlugInExtensionInstance * _Nonnull ARA_CALL ARAIPCAUProxyPlugInBindToDocumentController (AUAudioUnit * _Nonnull audioUnit,
                                                                                                  ARADocumentControllerRef _Nonnull documentControllerRef,
                                                                                                  ARAPlugInInstanceRoleFlags knownRoles,
                                                                                                  ARAPlugInInstanceRoleFlags assignedRoles)
{
    static_assert (sizeof (AUAudioUnit *) == sizeof (NSUInteger), "opaque ref type size mismatch");
    auto remoteInstance { static_cast<NSNumber *> ([audioUnit valueForKey:@"araRemoteInstanceRef"]) };
    auto remoteInstanceRef { reinterpret_cast<ARAIPCPlugInInstanceRef> ([remoteInstance unsignedIntegerValue]) };
    const auto plugInExtensionInstance { ARAIPCProxyPlugInBindToDocumentController (remoteInstanceRef, documentControllerRef, knownRoles, assignedRoles) };
    return plugInExtensionInstance;
}

void ARA_CALL ARAIPCAUProxyPlugInUninitialize (ARAIPCConnectionRef _Nonnull proxyRef)
{
    delete fromIPCRef (fromIPCRef (proxyRef)->mainChannelRef);
    delete fromIPCRef (proxyRef);
}



// plug-in side: proxy host C adapter
ProxyHostMessageChannel * _factoryMessageChannel {};

const ARAPlugInExtensionInstance * ARA_CALL ARAIPCAUBindingHandler (ARAIPCPlugInInstanceRef plugInInstanceRef,
                                                                    ARADocumentControllerRef controllerRef,
                                                                    ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles)
{
    auto audioUnit { (__bridge AUAudioUnit<ARAAudioUnit> *) plugInInstanceRef };
    return [audioUnit bindToDocumentController:controllerRef withRoles:assignedRoles knownRoles:knownRoles];
}

void ARA_CALL ARAIPCAUProxyHostInitialize (NSObject<AUMessageChannel> * _Nonnull factoryMessageChannel)
{
    _factoryMessageChannel = new ProxyHostMessageChannel { factoryMessageChannel };

    ARAIPCProxyHostSetBindingHandler (ARAIPCAUBindingHandler);
}

ARAIPCMessageChannelRef _Nullable ARA_CALL ARAIPCAUProxyHostInitializeMessageChannel (NSObject<AUMessageChannel> * _Nonnull audioUnitChannel)
{
    return toIPCRef (new ProxyHostMessageChannel { audioUnitChannel });
}

NSDictionary * _Nonnull ARA_CALL ARAIPCAUProxyHostCommandHandler (ARAIPCMessageChannelRef _Nonnull messageChannelRef, NSDictionary * _Nonnull message)
{
    static_cast<ProxyHostMessageChannel *> (fromIPCRef (messageChannelRef))->routeReceivedMessage (message);
    return [NSDictionary dictionary];   // \todo it would yield better performance if -callAudioUnit: would allow nil as return value
}

void ARA_CALL ARAIPCAUProxyHostUninitializeMessageChannel (ARAIPCMessageChannelRef _Nonnull messageChannelRef)
{
    delete fromIPCRef (messageChannelRef);
}

void ARA_CALL ARAIPCAUProxyHostUninitialize (void)
{
    delete _factoryMessageChannel;
}


API_AVAILABLE_END


_Pragma ("GCC diagnostic pop")


}   // extern "C"
}   // namespace IPC
}   // namespace ARA


#endif // ARA_AUDIOUNITV3_IPC_IS_AVAILABLE
