//------------------------------------------------------------------------------
//! \file       ARAIPCAudioUnit_v3.m
//!             Implementation of ARA IPC message sending through AUMessageChannel
//! \project    ARA SDK Library
//! \copyright  Copyright (c) 2021-2026, Celemony Software GmbH, All Rights Reserved.
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
#import "ARA_Library/IPC/ARAIPCConnection.h"
#import "ARA_Library/IPC/ARAIPCProxyHost.h"
#import "ARA_Library/IPC/ARAIPCProxyPlugIn.h"
#if !ARA_ENABLE_IPC
    #error "configuration mismatch: enabling ARA_AUDIOUNITV3_IPC_IS_AVAILABLE requires enabling ARA_ENABLE_IPC too"
#endif

#include <atomic>


// JUCE hotfix: because JUCE directly includes the .cpp/.mm files from this SDK instead of properly
//              compiling the ARA_IPC_Library, these switches allow for skipping the host- or the
//              plug-in side of the code, depending on which side is being compiled.
#if !defined(ARA_AUDIOUNITV3_IPC_PROXY_HOST_ONLY)
    #define ARA_AUDIOUNITV3_IPC_PROXY_HOST_ONLY 0
#endif
#if !defined(ARA_AUDIOUNITV3_IPC_PROXY_PLUGIN_ONLY)
    #define ARA_AUDIOUNITV3_IPC_PROXY_PLUGIN_ONLY 0
#endif


namespace ARA {
namespace IPC {


_Pragma ("GCC diagnostic push")
_Pragma ("GCC diagnostic ignored \"-Wold-style-cast\"")             // __bridge casts can only be done old-style
_Pragma ("GCC diagnostic ignored \"-Wnullability-completeness\"")   // \todo add proper nullability annotation


API_AVAILABLE_BEGIN(macos(13.0), ios(16.0))


// key for transaction locking through the IPC channel
constexpr NSString * _messageIDKey { @"msgID" };


// connection implementation for both proxy implementations
class AUConnection : public Connection
{
public:
    using Connection::Connection;

    MessageEncoder * createEncoder () override
    {
        return new CFMessageEncoder {};
    }

    bool receiverEndianessMatches () override
    {
        // \todo shouldn't the AUMessageChannel provide this information?
        return true;
    }

    void performPendingMainThreadTasks ()
    {
        getMainThreadDispatcher ()->processPendingMessageIfNeeded ();
    }
};


// message channel base class for both proxy implementations
class AudioUnitMessageChannel : public MessageChannel
{
public:
    AudioUnitMessageChannel (NSObject<AUMessageChannel> * _Nonnull audioUnitChannel)
    : _audioUnitChannel { audioUnitChannel }
    {
#if !__has_feature(objc_arc)
        [_audioUnitChannel retain];
#endif
    }

#if !__has_feature(objc_arc)
    ~AudioUnitMessageChannel () override
    {
        [_audioUnitChannel release];
    }
#endif

public:
    void routeReceivedMessage (NSDictionary * _Nonnull message)
    {
        ARA_INTERNAL_ASSERT (![NSThread isMainThread]);
        const MessageID messageID { [(NSNumber *) [message objectForKey:_messageIDKey] intValue] };
        const auto decoder { new CFMessageDecoder { (__bridge CFDictionaryRef) message } };
        getMessageDispatcher ()->routeReceivedMessage (messageID, decoder);
    }

    void sendMessage (MessageID messageID, MessageEncoder * encoder) override
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
#if !ARA_AUDIOUNITV3_IPC_PROXY_PLUGIN_ONLY
class ProxyHostMessageChannel : public AudioUnitMessageChannel
{
public:
    using AudioUnitMessageChannel::AudioUnitMessageChannel;

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
#endif  // !ARA_AUDIOUNITV3_IPC_PROXY_PLUGIN_ONLY


// host side: proxy plug-in message channel specialization
#if !ARA_AUDIOUNITV3_IPC_PROXY_HOST_ONLY
class ProxyPlugInMessageChannel : public AudioUnitMessageChannel
{
public:
    ProxyPlugInMessageChannel (NSObject<AUMessageChannel> * _Nonnull audioUnitChannel)
    : AudioUnitMessageChannel { audioUnitChannel }
    {
        _audioUnitChannel.callHostBlock =
            ^NSDictionary * _Nullable (NSDictionary * _Nonnull message)
            {
                routeReceivedMessage (message);
                return [NSDictionary dictionary];   // \todo it would yield better performance if the callHostBlock would allow nil as return value
            };

        // \todo macOS as of 14.3.1 does not forward the above assignment of callHostBlock to the
        //       remote side until the first message is sent. However, if another channel is assigned
        //       its (same) callHostBlock, then the pending callHostBlock for the first channel is
        //       lost for some reason. We need to send an empty dummy message to work around this bug,
        //       which them must be filtered in ARAIPCAUProxyHostCommandHandler().
        [audioUnitChannel callAudioUnit:[NSDictionary dictionary]];
    }

    ~ProxyPlugInMessageChannel () override
    {
        _audioUnitChannel.callHostBlock = nil;
    }

protected:
    NSDictionary * _sendMessage (NSDictionary * message) override
    {
        const auto reply { [_audioUnitChannel callAudioUnit:message] };
        return reply;
    }
};
#endif // !ARA_AUDIOUNITV3_IPC_PROXY_HOST_ONLY

// host side: proxy plug-in implementation
#if !ARA_AUDIOUNITV3_IPC_PROXY_HOST_ONLY
class AUProxyPlugIn : public ProxyPlugIn, public AUConnection
{
public:
    static AUProxyPlugIn* createWithAudioUnit (AUAudioUnit * _Nonnull audioUnit,
                                               ARAMainThreadWaitForMessageDelegate _Nullable waitForMessageDelegate,
                                               void * _Nullable delegateUserData)
    {
        // AUAudioUnits created before macOS 13 will not know about this API yet
        if (![audioUnit respondsToSelector:@selector(messageChannelFor:)])
            return nullptr;

        auto mainChannel { [audioUnit messageChannelFor:ARA_AUDIOUNIT_MAIN_THREAD_MESSAGES_UTI] };
        if (!mainChannel)
            return nullptr;

        auto otherChannel { [audioUnit messageChannelFor:ARA_AUDIOUNIT_OTHER_THREADS_MESSAGES_UTI] };
        if (!otherChannel)
            return nullptr;

        return new AUProxyPlugIn { static_cast<NSObject<AUMessageChannel>*> (mainChannel),
                                   static_cast<NSObject<AUMessageChannel>*> (otherChannel),
                                   audioUnit, waitForMessageDelegate, delegateUserData };
    }

    ~AUProxyPlugIn () override
    {
#if !__has_feature(objc_arc)
        [_initAU release];
#endif
    }

    bool sendsHostMessages () const override { return true; }

private:
    AUProxyPlugIn (NSObject<AUMessageChannel> * _Nonnull mainChannel,
                   NSObject<AUMessageChannel> * _Nonnull otherChannel,
                   AUAudioUnit * _Nonnull initAU,
                   ARAMainThreadWaitForMessageDelegate _Nullable waitForMessageDelegate,
                   void * _Nullable delegateUserData)
    : ProxyPlugIn { this },
      AUConnection { waitForMessageDelegate, delegateUserData },
      _initAU { initAU }
    {
        setMainThreadChannel (new ProxyPlugInMessageChannel { mainChannel });
        setOtherThreadsChannel (new ProxyPlugInMessageChannel { otherChannel });
        setMessageHandler (this);
#if !__has_feature(objc_arc)
        [_initAU retain];
#endif
    }

private:
    const AUAudioUnit * __strong _initAU;   // workaround for macOS 14: keep the AU that vends the message channels alive, otherwise the channels will eventually stop working
                                            // \todo once this is fixed in macOS, we only need to store this on older macOS versions
};
#endif // !ARA_AUDIOUNITV3_IPC_PROXY_HOST_ONLY


#if defined (__GNUC__)
    _Pragma ("GCC diagnostic push")
    _Pragma ("GCC diagnostic ignored \"-Wunguarded-availability\"")
#endif

ARA_MAP_IPC_REF (AudioUnitMessageChannel, ARAIPCMessageChannelRef)
ARA_MAP_IPC_REF (AUConnection, ARAIPCConnectionRef)

#if defined (__GNUC__)
    _Pragma ("GCC diagnostic pop")
#endif


extern "C" {


// host side: proxy plug-in C adapter
#if !ARA_AUDIOUNITV3_IPC_PROXY_HOST_ONLY

ARAIPCConnectionRef ARA_CALL ARAIPCAUProxyPlugInInitialize (AUAudioUnit * _Nonnull audioUnit,
                                                            ARAMainThreadWaitForMessageDelegate _Nullable waitForMessageDelegate,
                                                            void * _Nullable delegateUserData)
{
    return toIPCRef (AUProxyPlugIn::createWithAudioUnit (audioUnit, waitForMessageDelegate, delegateUserData));
}

void ARA_CALL ARAIPCAUProxyPlugInPerformPendingMainThreadTasks (ARAIPCConnectionRef _Nonnull proxyRef)
{
    fromIPCRef (proxyRef)->performPendingMainThreadTasks ();
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
    delete fromIPCRef (proxyRef);
}

#endif // !ARA_AUDIOUNITV3_IPC_PROXY_HOST_ONLY


// plug-in side: proxy host C adapter
#if !ARA_AUDIOUNITV3_IPC_PROXY_PLUGIN_ONLY

class AUProxyHost : public ProxyHost, public AUConnection
{
public:
    AUProxyHost ()
    : ProxyHost { this },
      AUConnection { nullptr, nullptr }
    {
        ARAIPCProxyHostSetBindingHandler (handleBinding);
        setMessageHandler (this);
    }

    bool sendsHostMessages () const override { return false; }

private:
    static const ARAPlugInExtensionInstance * ARA_CALL handleBinding (ARAIPCPlugInInstanceRef plugInInstanceRef,
                                                                      ARADocumentControllerRef controllerRef,
                                                                      ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles)
    {
        auto audioUnit { (__bridge AUAudioUnit<ARAAudioUnit> *) plugInInstanceRef };
        return [audioUnit bindToDocumentController:controllerRef withRoles:assignedRoles knownRoles:knownRoles];
    }
};

AUProxyHost* _proxyHost;


ARAIPCMessageChannelRef _Nullable ARA_CALL ARAIPCAUProxyHostInitializeMessageChannel (NSObject<AUMessageChannel> * _Nonnull audioUnitChannel,
                                                                                      bool isMainThreadChannel)
{
    // \todo the connection currently stores the creation thread as main thread for Windows compatibility,
    //       so we need to make sure the proxy is created on the main thread
    auto createProxyIfNeeded {
             ^void ()
             {
                 if (!_proxyHost)
                     _proxyHost = new AUProxyHost {};
             }};
     
     if ([NSThread isMainThread])
         createProxyIfNeeded ();
     else
         dispatch_sync (dispatch_get_main_queue (), createProxyIfNeeded);

    auto result { new ProxyHostMessageChannel { audioUnitChannel } };
    if (isMainThreadChannel)
        _proxyHost->setMainThreadChannel (result);
    else
        _proxyHost->setOtherThreadsChannel (result);

    return toIPCRef (result);
}

NSDictionary * _Nonnull ARA_CALL ARAIPCAUProxyHostCommandHandler (ARAIPCMessageChannelRef _Nonnull messageChannelRef, NSDictionary * _Nonnull message)
{
    if ([message count])    // \todo filter dummy message sent in ProxyPlugInMessageChannel(), see there
        static_cast<ProxyHostMessageChannel *> (fromIPCRef (messageChannelRef))->routeReceivedMessage (message);
    return [NSDictionary dictionary];   // \todo it would yield better performance if -callAudioUnit: would allow nil as return value
}

void ARA_CALL ARAIPCAUProxyHostUninitializeMessageChannel (ARAIPCMessageChannelRef _Nonnull messageChannelRef)
{
    delete fromIPCRef (messageChannelRef);
}

void ARA_CALL ARAIPCAUProxyHostUninitialize (void)
{
    if (_proxyHost)
        delete _proxyHost;
}

#endif // !ARA_AUDIOUNITV3_IPC_PROXY_PLUGIN_ONLY

API_AVAILABLE_END


_Pragma ("GCC diagnostic pop")


}   // extern "C"
}   // namespace IPC
}   // namespace ARA


#endif // ARA_AUDIOUNITV3_IPC_IS_AVAILABLE
