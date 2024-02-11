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
#import "ARA_Library/IPC/ARAIPCMessageChannel.h"
#import "ARA_Library/IPC/ARAIPCProxyHost.h"
#import "ARA_Library/IPC/ARAIPCProxyPlugIn.h"
#if !ARA_ENABLE_IPC
    #error "configuration mismatch: enabling ARA_AUDIOUNITV3_IPC_IS_AVAILABLE requires enabling ARA_ENABLE_IPC too"
#endif

#include <atomic>


namespace ARA {
namespace IPC {
extern "C" {


_Pragma ("GCC diagnostic push")
_Pragma ("GCC diagnostic ignored \"-Wold-style-cast\"") // __bridge casts can only be done old-style


API_AVAILABLE_BEGIN(macos(13.0))


// key for transaction locking through the IPC channel
constexpr NSString * _messageIDKey { @"msgID" };
constexpr NSString * _transactionLockKey { @"transactionLock" };


// message channel base class for both proxy implementations
class ARAIPCAUMessageChannel : public ARAIPCMessageChannel
{
protected:
    ARAIPCAUMessageChannel (NSObject<AUMessageChannel> * _Nonnull audioUnitChannel, ARAIPCMessageHandler * messageHandler)
    : ARAIPCMessageChannel { messageHandler },
      _audioUnitChannel { audioUnitChannel }
    {}

public:
    ARAIPCMessageEncoder * createEncoder () override
    {
        return ARAIPCCFCreateMessageEncoder ();
    }

    bool receiverEndianessMatches () override
    {
        // \todo shouldn't the AUMessageChannel provide this information?
        return true;
    }

    void routeReceivedMessage (NSDictionary * _Nonnull message)
    {
        const ARAIPCMessageID messageID { [(NSNumber *) [message objectForKey:_messageIDKey] intValue] };
        ARAIPCMessageDecoder* decoder {};
        if ([message count] > 1)
            decoder = ARAIPCCFCreateMessageDecoderWithDictionary ((__bridge CFDictionaryRef) message);
        ARAIPCMessageChannel::routeReceivedMessage (messageID, decoder);
    }

    void _sendMessage (ARAIPCMessageID messageID, ARAIPCMessageEncoder * encoder) override
    {
#if !__has_feature(objc_arc)
        auto message { (__bridge NSMutableDictionary *) ARAIPCCFCopyMessageEncoderDictionary (encoder) };
#else
        auto message { (__bridge_transfer NSMutableDictionary *) ARAIPCCFCopyMessageEncoderDictionary (encoder) };
#endif
        [message setObject:[NSNumber numberWithInt: messageID] forKey:_messageIDKey];
        const auto reply { _sendMessage (message) };
        ARA_INTERNAL_ASSERT (([reply count] == 0) || (([reply count] == 1) && [message objectForKey:_transactionLockKey]));
#if !__has_feature(objc_arc)
        [message release];
#endif
    }

protected:
    virtual NSDictionary * _sendMessage (NSDictionary * message) = 0;

    NSObject<AUMessageChannel> * _Nonnull getAudioUnitChannel ()
    {
        return _audioUnitChannel;
    }

private:
    NSObject<AUMessageChannel> * __unsafe_unretained _Nonnull _audioUnitChannel;    // avoid "retain" cycle: the AUMessageChannel implementation manages this object
};


// plug-in side: proxy host message channel specialization
class ARAIPCAUHostMessageChannel : public ARAIPCAUMessageChannel, public ARAIPCProxyHostMessageHandler
{
public:
    ARAIPCAUHostMessageChannel (NSObject<AUMessageChannel> * _Nonnull audioUnitChannel,
                                AUAudioUnit * _Nullable audioUnit)
    : ARAIPCAUMessageChannel { audioUnitChannel, this },
      _audioUnit { audioUnit },
      _sendLock { dispatch_semaphore_create (1) }
    {}

#if !__has_feature(objc_arc)
    ~ARAIPCAUHostMessageChannel () override
    {
        dispatch_release (_sendLock);
    }
#endif

    AUAudioUnit * _Nullable getAudioUnit ()
    {
        return _audioUnit;
    }

protected:
    NSDictionary * _sendMessage (NSDictionary * message) override
    {
        if (const auto callHostBlock { getAudioUnitChannel ().callHostBlock })
        {
            dispatch_semaphore_wait (_sendLock, DISPATCH_TIME_FOREVER);
            NSDictionary * reply { callHostBlock (message) };
            dispatch_semaphore_signal (_sendLock);
            return reply;
        }
        else
        {
            ARA_INTERNAL_ASSERT (false && "trying to send IPC message while host has not set callHostBlock");
            return nil;
        }
    }

public:
    void lockTransaction () override
    {
        const auto message { [NSDictionary dictionaryWithObject:[NSNumber numberWithBool:true] forKey:_transactionLockKey] };
        while (true)
        {
            const auto reply { _sendMessage (message) };
            ARA_INTERNAL_ASSERT ([reply count] == 1);
            ARA_INTERNAL_ASSERT ([reply objectForKey:_transactionLockKey] != nil);
            if ([(NSNumber *) [reply objectForKey:_transactionLockKey] boolValue])
                break;

            // \todo in order to avoid this busy-wait, the other side would need to send us some "lock available" message
            //       upon unlock whenever a lock request was rejected - this thread could wait via some signal then retry...
            std::this_thread::sleep_for (std::chrono::microseconds { 100 });
        }
        std::atomic_thread_fence (std::memory_order_acquire);
    }

    void unlockTransaction () override
    {
        std::atomic_thread_fence (std::memory_order_release);
        const auto message { [NSDictionary dictionaryWithObject:[NSNumber numberWithBool:false] forKey:_transactionLockKey] };
        const auto reply { _sendMessage (message) };
        ARA_INTERNAL_ASSERT ([reply count] == 1);
        ARA_INTERNAL_ASSERT ([reply objectForKey:_transactionLockKey] != nil);
        ARA_INTERNAL_ASSERT ([(NSNumber *) [reply objectForKey:_transactionLockKey] boolValue]);
    }

private:
    AUAudioUnit * __unsafe_unretained _Nullable _audioUnit;
    dispatch_semaphore_t _sendLock;     // needed because we're injecting messages from any thread in order to access the transaction lock
};


// host side: proxy plug-in message channel specialization
class ARAIPCAUPlugInMessageChannel : public ARAIPCAUMessageChannel, public ARAIPCProxyPlugInMessageHandler
{
public:
    ARAIPCAUPlugInMessageChannel (NSObject<AUMessageChannel> * _Nonnull audioUnitChannel)
    : ARAIPCAUMessageChannel { audioUnitChannel, this },
      _transactionLock { dispatch_semaphore_create (1) }
    {
        getAudioUnitChannel ().callHostBlock =
            ^NSDictionary * _Nullable (NSDictionary * _Nonnull message)
            {
                if (const auto transactionLockRequest { (NSNumber *) [message objectForKey:_transactionLockKey] })
                {
                    bool result { true };
                    if ([transactionLockRequest boolValue])
                        result = (dispatch_semaphore_wait (_transactionLock, DISPATCH_TIME_NOW) == 0);
                    else
                        dispatch_semaphore_signal (_transactionLock);
                    return [NSDictionary dictionaryWithObject:[NSNumber numberWithBool:result] forKey:_transactionLockKey];
                }

                routeReceivedMessage (message);
                return [NSDictionary dictionary];   // \todo it would yield better performance if the callHostBlock would allow nil as return value
            };
    }

    ~ARAIPCAUPlugInMessageChannel () override
    {
        getAudioUnitChannel ().callHostBlock = nil;
#if !__has_feature(objc_arc)
        dispatch_release (_transactionLock);
#endif
    }

protected:
    NSDictionary * _sendMessage (NSDictionary * message) override
    {
        const auto reply { [getAudioUnitChannel () callAudioUnit:message] };
        return reply;
    }

public:
    void lockTransaction () override
    {
        const auto ARA_MAYBE_UNUSED_VAR (result) { dispatch_semaphore_wait (_transactionLock, DISPATCH_TIME_FOREVER) };
        ARA_INTERNAL_ASSERT (result == 0);
    }

    void unlockTransaction () override
    {
        dispatch_semaphore_signal (_transactionLock);
    }

private:
    dispatch_semaphore_t _transactionLock;
};


// host side: proxy plug-in message channel further specialization for plug-in extension messages
class ARAIPCAUPlugInExtensionMessageChannel : public ARAIPCAUPlugInMessageChannel
{
private:
    using ARAIPCAUPlugInMessageChannel::ARAIPCAUPlugInMessageChannel;

public:
    static ARAIPCAUPlugInExtensionMessageChannel * bindAndCreateMessageChannel (NSObject<AUMessageChannel> * _Nonnull audioUnitChannel,
                                                                                ARADocumentControllerRef _Nonnull documentControllerRef,
                                                                                ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles)
    {
        auto result { new ARAIPCAUPlugInExtensionMessageChannel { audioUnitChannel } };
        result->_plugInExtensionInstance = ARAIPCProxyPlugInBindToDocumentController (0, result, documentControllerRef, knownRoles, assignedRoles);
        return result;
    }

    ~ARAIPCAUPlugInExtensionMessageChannel () override
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



// host side: proxy plug-in C adapter
NSObject<AUMessageChannel> * _Nullable ARAIPCAUGetMessageChannel (AUAudioUnit * _Nonnull audioUnit, NSString * _Nonnull identifier)
{
    // AUAudioUnits created before macOS 13 will not know about this API yet
    if (![audioUnit respondsToSelector:@selector(messageChannelFor:)])
        return nil;

    return (NSObject<AUMessageChannel> *) [audioUnit messageChannelFor:identifier];
}

ARAIPCMessageChannel * _Nullable ARA_CALL ARAIPCAUProxyPlugInInitializeFactoryMessageChannel (AUAudioUnit * _Nonnull audioUnit)
{
    auto factoryChannel { ARAIPCAUGetMessageChannel (audioUnit, ARA_AUDIOUNIT_FACTORY_CUSTOM_MESSAGES_UTI) };
    if (!factoryChannel)
        return nullptr;

    return new ARAIPCAUPlugInMessageChannel { (NSObject<AUMessageChannel> * _Nonnull) factoryChannel };
}

const ARAFactory * _Nonnull ARA_CALL ARAIPCAUProxyPlugInGetFactory (ARAIPCMessageChannel * _Nonnull messageChannel)
{
    ARA_VALIDATE_API_CONDITION (ARAIPCProxyPlugInGetFactoriesCount (messageChannel) == 1);
    const ARAFactory * result { ARAIPCProxyPlugInGetFactoryAtIndex (messageChannel, 0) };
    ARA_VALIDATE_API_CONDITION (result != nullptr);
    return result;
}

const ARAPlugInExtensionInstance * _Nullable ARA_CALL ARAIPCAUProxyPlugInBindToDocumentController (AUAudioUnit * _Nonnull audioUnit,
                                                                                                   ARADocumentControllerRef _Nonnull documentControllerRef,
                                                                                                   ARAPlugInInstanceRoleFlags knownRoles,
                                                                                                   ARAPlugInInstanceRoleFlags assignedRoles,
                                                                                                   ARAIPCMessageChannel * _Nullable * _Nonnull messageChannel)
{
    auto audioUnitChannel { ARAIPCAUGetMessageChannel (audioUnit, ARA_AUDIOUNIT_PLUGINEXTENSION_CUSTOM_MESSAGES_UTI) };
    if (!audioUnitChannel)
    {
        *messageChannel = nullptr;
        return nullptr;
    }

    *messageChannel = ARAIPCAUPlugInExtensionMessageChannel::bindAndCreateMessageChannel ((NSObject<AUMessageChannel> * _Nonnull) audioUnitChannel,
                                                                                          documentControllerRef, knownRoles, assignedRoles);
    return static_cast<ARAIPCAUPlugInExtensionMessageChannel *> (*messageChannel)->getPlugInExtensionInstance ();
}

void ARA_CALL ARAIPCAUProxyPlugInCleanupBinding (ARAIPCMessageChannel * messageChannel)
{
    delete messageChannel;
}

void ARA_CALL ARAIPCAUProxyPlugInUninitializeFactoryMessageChannel (ARAIPCMessageChannel * _Nonnull messageChannel)
{
    delete messageChannel;
}



// plug-in side: proxy host C adapter
ARAIPCAUHostMessageChannel * _factoryMessageChannel {};

void ARA_CALL ARAIPCAUProxyHostAddFactory (const ARAFactory * _Nonnull factory)
{
    ARAIPCProxyHostAddFactory (factory);
}

const ARAPlugInExtensionInstance * ARA_CALL ARAIPCAUBindingHandler (ARAIPCMessageChannel * messageChannel, ARAIPCPlugInInstanceRef /*plugInInstanceRef*/,
                                                                    ARADocumentControllerRef controllerRef,
                                                                    ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles)
{
    auto audioUnit { static_cast<ARAIPCAUHostMessageChannel *>(messageChannel)->getAudioUnit () };
    return [(AUAudioUnit<ARAAudioUnit> *) audioUnit bindToDocumentController:controllerRef withRoles:assignedRoles knownRoles:knownRoles];
}

void ARA_CALL ARAIPCAUProxyHostInitialize (NSObject<AUMessageChannel> * _Nonnull factoryMessageChannel)
{
    _factoryMessageChannel = new ARAIPCAUHostMessageChannel { factoryMessageChannel, nil };

    ARAIPCProxyHostSetBindingHandler (ARAIPCAUBindingHandler);
}

ARAIPCMessageChannel * _Nullable ARA_CALL ARAIPCAUProxyHostInitializeMessageChannel (AUAudioUnit * _Nonnull audioUnit,
                                                                                     NSObject<AUMessageChannel> * _Nonnull audioUnitChannel)
{
    return new ARAIPCAUHostMessageChannel { audioUnitChannel, audioUnit };
}

NSDictionary * _Nonnull ARA_CALL ARAIPCAUProxyHostCommandHandler (ARAIPCMessageChannel * _Nonnull messageChannel, NSDictionary * _Nonnull message)
{
    ((ARAIPCAUHostMessageChannel *) messageChannel)->routeReceivedMessage (message);
    return [NSDictionary dictionary];   // \todo it would yield better performance if -callAudioUnit: would allow nil as return value
}

void ARA_CALL ARAIPCAUProxyHostUninitializeMessageChannel (ARAIPCMessageChannel * _Nonnull messageChannel)
{
    delete messageChannel;
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
