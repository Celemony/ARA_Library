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
    AudioUnitMessageChannel (MessageHandler * messageHandler)
    : MessageChannel { messageHandler }
    {}

public:
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
};


#if defined (__GNUC__)
    _Pragma ("GCC diagnostic push")
    _Pragma ("GCC diagnostic ignored \"-Wunguarded-availability\"")
#endif

ARA_MAP_IPC_REF (AudioUnitMessageChannel, ARA::IPC::ARAIPCMessageChannelRef)

#if defined (__GNUC__)
    _Pragma ("GCC diagnostic pop")
#endif


// plug-in side: proxy host message channel specialization
class ProxyHostMessageChannel : public AudioUnitMessageChannel, public ProxyHostMessageHandler
{
public:
    ProxyHostMessageChannel (NSObject<AUMessageChannel> * _Nonnull audioUnitChannel,
                             AUAudioUnit * _Nullable audioUnit)
    : AudioUnitMessageChannel { this },
      _audioUnitChannel { audioUnitChannel },
      _audioUnit { audioUnit }
    {}

    AUAudioUnit * _Nullable getAudioUnit ()
    {
        return _audioUnit;
    }

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

private:
    NSObject<AUMessageChannel> * __unsafe_unretained _Nonnull _audioUnitChannel;    // avoid retain cycle: the AUMessageChannel implementation manages this object
    AUAudioUnit * __unsafe_unretained _Nullable _audioUnit;
};


// host side: proxy plug-in message channel specialization
class ProxyPlugInMessageChannel : public AudioUnitMessageChannel, public ProxyPlugInMessageHandler
{
public:
    ProxyPlugInMessageChannel (NSObject<AUMessageChannel> * _Nonnull audioUnitChannel)
    : AudioUnitMessageChannel { this },
      _audioUnitChannel { audioUnitChannel }
    {
#if !__has_feature(objc_arc)
        [_audioUnitChannel retain];
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
        [_audioUnitChannel release];
#endif
    }

protected:
    NSDictionary * _sendMessage (NSDictionary * message) override
    {
        const auto reply { [_audioUnitChannel callAudioUnit:message] };
        return reply;
    }

private:
    NSObject<AUMessageChannel> * __strong _Nonnull _audioUnitChannel;
};


// host side: proxy plug-in message channel further specialization for plug-in extension messages
class ProxyPlugInExtensionMessageChannel : public ProxyPlugInMessageChannel
{
private:
    using ProxyPlugInMessageChannel::ProxyPlugInMessageChannel;

public:
    static ProxyPlugInExtensionMessageChannel * bindAndCreateMessageChannel (NSObject<AUMessageChannel> * _Nonnull audioUnitChannel,
                                                                             ARADocumentControllerRef _Nonnull documentControllerRef,
                                                                             ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles)
    {
        auto messageChannel { new ProxyPlugInExtensionMessageChannel { audioUnitChannel } };
        messageChannel->_plugInExtensionInstance = ARAIPCProxyPlugInBindToDocumentController (nullptr, toIPCRef (messageChannel), documentControllerRef, knownRoles, assignedRoles);
        return messageChannel;
    }

    ~ProxyPlugInExtensionMessageChannel () override
    {
        if (_plugInExtensionInstance)
            ARAIPCProxyPlugInCleanupBinding (_plugInExtensionInstance);
    }

    const ARAPlugInExtensionInstance * getPlugInExtensionInstance () const
    {
        return _plugInExtensionInstance;
    }

private:
    const ARAPlugInExtensionInstance * _plugInExtensionInstance {};
};



extern "C" {


// host side: proxy plug-in C adapter
NSObject<AUMessageChannel> * _Nullable ARAIPCAUGetMessageChannel (AUAudioUnit * _Nonnull audioUnit, NSString * _Nonnull identifier)
{
    // AUAudioUnits created before macOS 13 will not know about this API yet
    if (![audioUnit respondsToSelector:@selector(messageChannelFor:)])
        return nil;

    return (NSObject<AUMessageChannel> *) [audioUnit messageChannelFor:identifier];
}

ARAIPCMessageChannelRef _Nullable ARA_CALL ARAIPCAUProxyPlugInInitializeFactoryMessageChannel (AUAudioUnit * _Nonnull audioUnit)
{
    auto factoryChannel { ARAIPCAUGetMessageChannel (audioUnit, ARA_AUDIOUNIT_FACTORY_CUSTOM_MESSAGES_UTI) };
    if (!factoryChannel)
        return nullptr;

    return toIPCRef (new ProxyPlugInMessageChannel { (NSObject<AUMessageChannel> * _Nonnull) factoryChannel });
}

const ARAFactory * _Nonnull ARA_CALL ARAIPCAUProxyPlugInGetFactory (ARAIPCMessageChannelRef _Nonnull messageChannelRef)
{
    ARA_VALIDATE_API_CONDITION (ARAIPCProxyPlugInGetFactoriesCount (messageChannelRef) == 1);
    const ARAFactory * result { ARAIPCProxyPlugInGetFactoryAtIndex (messageChannelRef, 0) };
    ARA_VALIDATE_API_CONDITION (result != nullptr);
    return result;
}

const ARAPlugInExtensionInstance * _Nullable ARA_CALL ARAIPCAUProxyPlugInBindToDocumentController (AUAudioUnit * _Nonnull audioUnit,
                                                                                                   ARADocumentControllerRef _Nonnull documentControllerRef,
                                                                                                   ARAPlugInInstanceRoleFlags knownRoles,
                                                                                                   ARAPlugInInstanceRoleFlags assignedRoles,
                                                                                                   ARAIPCMessageChannelRef _Nullable * _Nonnull messageChannelRef)
{
    auto audioUnitChannel { ARAIPCAUGetMessageChannel (audioUnit, ARA_AUDIOUNIT_PLUGINEXTENSION_CUSTOM_MESSAGES_UTI) };
    if (!audioUnitChannel)
    {
        *messageChannelRef = nullptr;
        return nullptr;
    }

    const auto messageChannel { ProxyPlugInExtensionMessageChannel::bindAndCreateMessageChannel ((NSObject<AUMessageChannel> * _Nonnull) audioUnitChannel,
                                                                                                 documentControllerRef, knownRoles, assignedRoles) };
    *messageChannelRef = toIPCRef (messageChannel);
    return messageChannel->getPlugInExtensionInstance ();
}

void ARA_CALL ARAIPCAUProxyPlugInCleanupBinding (ARAIPCMessageChannelRef messageChannelRef)
{
    delete fromIPCRef (messageChannelRef);
}

void ARA_CALL ARAIPCAUProxyPlugInUninitializeFactoryMessageChannel (ARAIPCMessageChannelRef _Nonnull messageChannelRef)
{
    delete fromIPCRef (messageChannelRef);
}



// plug-in side: proxy host C adapter
ProxyHostMessageChannel * _factoryMessageChannel {};

void ARA_CALL ARAIPCAUProxyHostAddFactory (const ARAFactory * _Nonnull factory)
{
    ARAIPCProxyHostAddFactory (factory);
}

const ARAPlugInExtensionInstance * ARA_CALL ARAIPCAUBindingHandler (ARAIPCMessageChannelRef messageChannelRef, ARAIPCPlugInInstanceRef /*plugInInstanceRef*/,
                                                                    ARADocumentControllerRef controllerRef,
                                                                    ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles)
{
    auto audioUnit { static_cast<ProxyHostMessageChannel *> (fromIPCRef (messageChannelRef))->getAudioUnit () };
    return [(AUAudioUnit<ARAAudioUnit> *) audioUnit bindToDocumentController:controllerRef withRoles:assignedRoles knownRoles:knownRoles];
}

void ARA_CALL ARAIPCAUProxyHostInitialize (NSObject<AUMessageChannel> * _Nonnull factoryMessageChannel)
{
    _factoryMessageChannel = new ProxyHostMessageChannel { factoryMessageChannel, nil };

    ARAIPCProxyHostSetBindingHandler (ARAIPCAUBindingHandler);
}

ARAIPCMessageChannelRef _Nullable ARA_CALL ARAIPCAUProxyHostInitializeMessageChannel (AUAudioUnit * _Nonnull audioUnit,
                                                                                      NSObject<AUMessageChannel> * _Nonnull audioUnitChannel)
{
    return toIPCRef (new ProxyHostMessageChannel { audioUnitChannel, audioUnit });
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
