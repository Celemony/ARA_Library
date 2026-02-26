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

#import <objc/runtime.h>


// JUCE hotfix: because JUCE directly includes the .cpp/.mm files from this SDK instead of properly
//              compiling the ARA_IPC_Library, these switches allow for skipping the host- or the
//              plug-in side of the code, depending on which side is being compiled.
#if !defined(ARA_AUDIOUNITV3_IPC_PROXY_HOST_ONLY)
    #define ARA_AUDIOUNITV3_IPC_PROXY_HOST_ONLY 0
#endif
#if !defined(ARA_AUDIOUNITV3_IPC_PROXY_PLUGIN_ONLY)
    #define ARA_AUDIOUNITV3_IPC_PROXY_PLUGIN_ONLY 0
#endif


// crude hack to use std::bit_width in C++17
#if __cplusplus < 202002L
namespace std
{
    constexpr size_t bit_width (size_t n)
    {
        return (n <= 1) ? n : 1 + bit_width (n / 2);
    }
}
#endif


namespace ARA {
namespace IPC {


_Pragma ("GCC diagnostic push")
_Pragma ("GCC diagnostic ignored \"-Wold-style-cast\"")             // __bridge casts can only be done old-style
_Pragma ("GCC diagnostic ignored \"-Wnullability-completeness\"")   // \todo add proper nullability annotation


API_AVAILABLE_BEGIN(macos(13.0), ios(16.0))


// key to store message ID in XPC message
constexpr NSString * _messageIDKey { @"msgID" };


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
    void routeReceivedDictionary (NSDictionary * _Nonnull message)
    {
        ARA_INTERNAL_ASSERT (![NSThread isMainThread]);
        const MessageID messageID { [(NSNumber *) [message objectForKey:_messageIDKey] intValue] };
        auto decoder { std::make_unique<CFMessageDecoder> ((__bridge CFDictionaryRef) message) };
        routeReceivedMessage (messageID, std::move (decoder));
    }

    void sendMessage (MessageID messageID, std::unique_ptr<MessageEncoder> && encoder) override
    {
        const auto dictionary { static_cast<CFMessageEncoder *> (encoder.get ())->copyDictionary () };
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
                routeReceivedDictionary (message);
                return [NSDictionary dictionary];   // \todo it would yield better performance if the callHostBlock would allow nil as return value
            };

        // \todo macOS as of 14.3.1 does not forward the above assignment of callHostBlock to the
        //       remote side until the first message is sent. However, if another channel is assigned
        //       its (same) callHostBlock, then the pending callHostBlock for the first channel is
        //       lost for some reason. We need to send an empty dummy message to work around this bug,
        //       which them must be filtered before calling routeReceivedDictionary().
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
class AUProxyPlugIn : public ProxyPlugIn
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

    ~AUProxyPlugIn ()
    {
#if !__has_feature(objc_arc)
        [_initAU release];
#endif
    }

private:
    AUProxyPlugIn (NSObject<AUMessageChannel> * _Nonnull mainChannel,
                   NSObject<AUMessageChannel> * _Nonnull otherChannel,
                   AUAudioUnit * _Nonnull initAU,
                   ARAMainThreadWaitForMessageDelegate _Nullable waitForMessageDelegate,
                   void * _Nullable delegateUserData)
    : ProxyPlugIn { std::make_unique<Connection> (&CFMessageEncoder::create,
                                                  ProxyPlugIn::handleReceivedMessage,
                                                  true, // \todo shouldn't the AUMessageChannel provide this information?
                                                  (waitForMessageDelegate) ?
                                                        [waitForMessageDelegate, delegateUserData] { waitForMessageDelegate(delegateUserData); } :
                                                        Connection::WaitForMessageDelegate {}) },
      _initAU { initAU }
    {
        getConnection ()->setMainThreadChannel (std::make_unique<ProxyPlugInMessageChannel> (mainChannel));
        getConnection ()->setOtherThreadsChannel (std::make_unique<ProxyPlugInMessageChannel> (otherChannel));
#if !__has_feature(objc_arc)
        [_initAU retain];
#endif
    }

private:
    const AUAudioUnit * __strong _initAU;   // workaround for macOS 14: keep the AU that vends the message channels alive, otherwise the channels will eventually stop working
                                            // \todo once this is fixed in macOS, we only need to store this on older macOS versions
};
#endif // !ARA_AUDIOUNITV3_IPC_PROXY_HOST_ONLY


// host side: proxy plug-in C adapter
#if !ARA_AUDIOUNITV3_IPC_PROXY_HOST_ONLY

ARA_MAP_IPC_REF (AUProxyPlugIn, ARAIPCProxyPlugInRef)

extern "C" {

ARAIPCProxyPlugInRef ARA_CALL ARAIPCAUProxyPlugInInitialize (AUAudioUnit * _Nonnull audioUnit,
                                                             ARAMainThreadWaitForMessageDelegate _Nullable waitForMessageDelegate,
                                                             void * _Nullable delegateUserData)
{
    return toIPCRef (AUProxyPlugIn::createWithAudioUnit (audioUnit, waitForMessageDelegate, delegateUserData));
}

void ARA_CALL ARAIPCAUProxyPlugInPerformPendingMainThreadTasks (ARAIPCProxyPlugInRef _Nonnull proxyPlugInRef)
{
    fromIPCRef (proxyPlugInRef)->getConnection ()->processPendingMessageOnCreationThreadIfNeeded ();
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

void ARA_CALL ARAIPCAUProxyPlugInUninitialize (ARAIPCProxyPlugInRef _Nonnull proxyPlugInRef)
{
    delete fromIPCRef (proxyPlugInRef);
}

}   // extern "C"

#endif // !ARA_AUDIOUNITV3_IPC_PROXY_HOST_ONLY


// plug-in side: proxy host C adapter
#if !ARA_AUDIOUNITV3_IPC_PROXY_PLUGIN_ONLY

class AUProxyHost : public ProxyHost
{
public:
    AUProxyHost ()
    : ProxyHost { std::make_unique<Connection> (&CFMessageEncoder::create,
                                                [this] (auto&& ...args) { handleReceivedMessage (args...); },
                                                true) } // \todo shouldn't the AUMessageChannel provide this information?                                                
    {
        ARAIPCProxyHostSetBindingHandler (handleBinding);
    }

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

typedef ARA_IPC_REF(ARAIPCMessageChannelRef);
ARA_MAP_IPC_REF (AudioUnitMessageChannel, ARAIPCMessageChannelRef)

static ARAIPCMessageChannelRef _Nullable initializeMessageChannel (NSObject<AUMessageChannel> * _Nonnull audioUnitChannel,
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

    auto channel { std::make_unique<ProxyHostMessageChannel> (audioUnitChannel) };
    const auto result { toIPCRef (channel.get ()) };

    if (isMainThreadChannel)
        _proxyHost->getConnection ()->setMainThreadChannel (std::move (channel));
    else
        _proxyHost->getConnection ()->setOtherThreadsChannel (std::move (channel));

    return result;
}

#endif // !ARA_AUDIOUNITV3_IPC_PROXY_PLUGIN_ONLY


// plug-in side: NSObject<AUMessageChannel> implementation
#if !ARA_AUDIOUNITV3_IPC_PROXY_PLUGIN_ONLY


/*
The following code implements the below class at runtime to work around potential ObjC class name conflicts

@interface ARAIPCAUMessageChannelImpl : NSObject<AUMessageChannel>
@end

@implementation ARAIPCAUMessageChannel {
    ARA::IPC::ARAIPCMessageChannelRef _messageChannelRef;
}

@synthesize callHostBlock = _callHostBlock;

- (instancetype)initAsMainThreadChannel:(BOOL) isMainThreadChannel {
    self = [super init];

    if (self == nil) { return nil; }

    _callHostBlock = nil;
    _messageChannelRef = initializeMessageChannel (self, isMainThreadChannel);

    return self;
}

- (NSDictionary * _Nonnull)callAudioUnit:(NSDictionary *)message {
    if ([message count])
        static_cast<ProxyHostMessageChannel *> (fromIPCRef (_messageChannelRef))->routeReceivedDictionary (message);
    return [NSDictionary dictionary];
}

@end
*/

// helper to add ivars to an ObjC class
template <typename T>
void addObjCIVar (Class cls, const char * name)
{
    constexpr auto size { sizeof (T) };
    constexpr auto alignment { static_cast<uint8_t> (std::bit_width (size)) };
    [[maybe_unused]] const auto success = class_addIvar (cls, name, size, alignment, @encode (T));
    ARA_INTERNAL_ASSERT (success);
}

// helpers to get/set a scalar ivar of an ObjC object
template <typename T>
inline T getScalarIVar (id self, Ivar ivar)
{
    const auto offset { ivar_getOffset (ivar) };
    const auto bytes = static_cast<const unsigned char *> ((__bridge void *) self);
    return *reinterpret_cast<const T *> (bytes + offset);
}

template <typename T>
inline void setScalarIVar (id self, Ivar ivar, T value)
{
    const auto offset { ivar_getOffset (ivar) };
    auto bytes = static_cast<unsigned char *> ((__bridge void *) self);
    *reinterpret_cast<T *> (bytes + offset) = value;
}


// optimization of object_getInstanceVariable: cache the ivars of our class
Ivar messageChannelRefIvar { nullptr };
Ivar callHostBlockIvar { nullptr };


// methods of our class
extern "C" {

static CallHostBlock callHostBlock_getter (id self, SEL /*_cmd*/)
{
    return object_getIvar (self, callHostBlockIvar);
}

static void callHostBlock_setter (id self, SEL /*_cmd*/, CallHostBlock newBlock)
{
    id oldBlock { object_getIvar (self, callHostBlockIvar) };
    if (oldBlock != newBlock)
    {
        if (oldBlock)
            Block_release ((__bridge void *)oldBlock);
        if (newBlock)
            newBlock = (__bridge CallHostBlock) Block_copy ((__bridge void *)newBlock);
        object_setIvar (self, callHostBlockIvar, newBlock);
    }
}

static NSDictionary * _Nonnull callAudioUnit_impl (id self, SEL /*_cmd*/, NSDictionary * message)
{
    const auto messageChannelRef { getScalarIVar<ARA::IPC::ARAIPCMessageChannelRef> (self, messageChannelRefIvar) };
    if ([message count])    // \todo filter dummy message sent in ProxyPlugInMessageChannel, see there
        static_cast<ProxyHostMessageChannel *> (fromIPCRef (messageChannelRef))->routeReceivedDictionary (message);
    return [NSDictionary dictionary];   // \todo it would yield better performance if -callAudioUnit: would allow nil as return value
}

}   // extern "C"


// creation of the ARAIPCAUMessageChannelImpl Class
static Class createMessageChannelObjCClass ()
{
    // create a unique class name by appending a unique address to the base name
    std::string className { "ARAIPCAUMessageChannelImpl" + std::to_string (reinterpret_cast<intptr_t> (&messageChannelRefIvar)) };
    ARA_INTERNAL_ASSERT (!objc_getClass (className.c_str ()));

    // allocate class
    Class cls { objc_allocateClassPair ([NSObject class], className.c_str (), 0) };
    ARA_INTERNAL_ASSERT (cls);

    // add iVars
    addObjCIVar<intptr_t> (cls, "_messageChannelRef");
    addObjCIVar<CallHostBlock> (cls, "_callHostBlock");

    // add properties
    objc_property_attribute_t attr[] { { "T", "@?" },               // type block
                                       { "C", "" },                 // C = copy
                                       { "V", "_callHostBlock" } }; // backing i-var
    [[maybe_unused]] auto success { class_addProperty (cls, "callHostBlock", attr, sizeof (attr) / sizeof (attr[0])) };

    // add getters/setters
    success = class_addMethod (cls, @selector(callHostBlock), (IMP)callHostBlock_getter, "@@:");
    ARA_INTERNAL_ASSERT (success);
    class_addMethod (cls, @selector(setCallHostBlock:), (IMP)callHostBlock_setter, "v@:@");
    ARA_INTERNAL_ASSERT (success);

    // add methods
    class_addMethod (cls, @selector(callAudioUnit:), (IMP)callAudioUnit_impl, "@@:@");
    ARA_INTERNAL_ASSERT (success);

    // declare conformance to <AUMessageChannel>
    const auto messageChannelProtocol { objc_getProtocol ("AUMessageChannel") };
    ARA_INTERNAL_ASSERT (messageChannelProtocol);
    success = class_addProtocol (cls, (Protocol * _Nonnull)messageChannelProtocol);
    ARA_INTERNAL_ASSERT (success);

    // activate class
    objc_registerClassPair (cls);

    // update cached ivar ptrs after activation
    messageChannelRefIvar = class_getInstanceVariable (cls, "_messageChannelRef");
    ARA_INTERNAL_ASSERT (messageChannelRefIvar);
    callHostBlockIvar = class_getInstanceVariable (cls, "_callHostBlock");
    ARA_INTERNAL_ASSERT (callHostBlockIvar);

    return cls;
}

// creation and designated initialisation of instances of ARAIPCAUMessageChannelImpl Class
static NSObject<AUMessageChannel> * createAndInitMessageChannel (Class cls, bool isMainThreadChannel)
{
    NSObject<AUMessageChannel> * obj { class_createInstance (cls, 0) };
    ARA_INTERNAL_ASSERT (obj);
    ARA_INTERNAL_ASSERT ([obj conformsToProtocol:@protocol(AUMessageChannel)]);
    obj = [obj init];
    ARA_INTERNAL_ASSERT (obj);

    object_setIvar (obj, callHostBlockIvar, nil);

    const auto messageChannelRef { initializeMessageChannel (obj, isMainThreadChannel) };
    setScalarIVar<ARA::IPC::ARAIPCMessageChannelRef> (obj, messageChannelRefIvar, messageChannelRef);

    return obj;
}


NSObject<AUMessageChannel> * __strong _mainMessageChannel { nil };
NSObject<AUMessageChannel> * __strong _otherMessageChannel { nil };

static void createSharedMessageChannels ()
{
    ARA_INTERNAL_ASSERT(!_mainMessageChannel && !_otherMessageChannel);

    const auto cls { createMessageChannelObjCClass () };
    _mainMessageChannel = createAndInitMessageChannel (cls, true);
    _otherMessageChannel = createAndInitMessageChannel (cls, false);
}

extern "C" id<AUMessageChannel> _Nullable ARA_CALL ARAIPCAUProxyHostMessageChannelFor (NSString * _Nonnull channelName)
{
    if ([channelName isEqualToString:ARA_AUDIOUNIT_MAIN_THREAD_MESSAGES_UTI])
    {
        if (!_mainMessageChannel)
            createSharedMessageChannels ();
        return _mainMessageChannel;
    }
    else if ([channelName isEqualToString:ARA_AUDIOUNIT_OTHER_THREADS_MESSAGES_UTI])
    {
        if (!_otherMessageChannel)
            createSharedMessageChannels ();
        return _otherMessageChannel;
    }
    return nil;
}

API_AVAILABLE_END

__attribute__((destructor))
static void destroySharedMessageChannelsIfNeeded ()
{
    if (@available(macOS 13.0, iOS 16.0, *))
    {
        if (_proxyHost)
            delete _proxyHost;

        _mainMessageChannel = nil;
        _otherMessageChannel = nil;
    }
}

#endif  // !ARA_AUDIOUNITV3_IPC_PROXY_PLUGIN_ONLY

_Pragma ("GCC diagnostic pop")


}   // namespace IPC
}   // namespace ARA


#endif // ARA_AUDIOUNITV3_IPC_IS_AVAILABLE
