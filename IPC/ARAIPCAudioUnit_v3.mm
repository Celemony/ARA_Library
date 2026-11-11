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
#import "ARA_Library/IPC/ARAIPCMessage.h"
#import "ARA_Library/IPC/ARAIPCConnection.h"
#import "ARA_Library/IPC/ARAIPCProxyHost.h"
#import "ARA_Library/IPC/ARAIPCProxyPlugIn.h"
#if !ARA_ENABLE_IPC
    #error "configuration mismatch: enabling ARA_AUDIOUNITV3_IPC_IS_AVAILABLE requires enabling ARA_ENABLE_IPC too"
#endif

#import "CoreFoundation/CoreFoundation.h"
#import <objc/runtime.h>
#import <Foundation/NSRunLoop.h>
#import <AppKit/NSApplication.h>

#include <atomic>

#if __cplusplus >= 202002L
    #include <bit>
#else
    // crude hack to use std::bit_width in C++17
    namespace std
    {
        constexpr size_t bit_width (size_t n)
        {
            return (n <= 1) ? n : 1 + bit_width (n / 2);
        }
    }
#endif


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


#if ARA_ENABLE_DEBUG_OUTPUT && 1
    #define ARA_RL_SYNC_LOG(...) ARA_LOG ("RL Sync " __VA_ARGS__)
    #define ARA_RL_MODE_AS_STRING(mode) ((CFStringGetLength (mode) == 0) ? "(none)" : CFStringGetCStringPtr (mode, kCFStringEncodingUTF8))
#else
    #define ARA_RL_SYNC_LOG(...) ((void) 0)
#endif


// RunLoop-based main thread sync design overview:
//
// Running a plug-in out-of-process results in the host and the plug-in each running their own main
// thread, which may execute concurrently with respect to each other. Any host<->plug-in communication
// that relies on the implicit synchronicity of the main thread when running in-process will break
// when running out-of-process unless the main threads are synchronized.
//
// The following code hooks into the run loop of both the host and the plug-in and uses a private
// run loop mode to block either side in alternating fashion.
// The host is the leader of the sync. After initialization, the plug-in will run in private mode
// in order to process any main thread calls that come in from the host via the IPC, but blocking
// any other events, timers etc.
// Whenever the host run loop begins waiting for an event or is exited, the host switches itself
// into private mode and sends an IPC message to the plug-in to run its loop in the mode that the
// host currently uses. Once the plug-in has processed all pending run loop sources and events,
// it'll switch back to private mode and sends a message back to the host so it can continue.
// Note that each time the run loop is executed, the code that it calls may run the loop recursively
// in another mode. This also will be communicated to the other side.
//
// There are various special cases to consider in addition to this general pattern:
// - Since it is activated via an IPC message, the run loop sync on the plug-in side does start
//   from within the run loop running in default mode. Therefore, instead of running the loop
//   recursively all times, it falls back to the underlying default mode if requested to run in
//   default mode at the lowest level.
// - Under certain circumstances, the ViewBridge seems to send events outside any run loop cycle.
//   In that case, NSRunLoop's currentMode will return nil, and we cannot sync.
//   It's possible that this causes problems in some scenarios in some plug-in/host combinations.
// - ViewBridge is special-casing modal operation, and syncing the run loops seems to break its
//   implementation at least in macOS 15
// - Audio Unit IPC calls are blocking the thread that they are made on. In order to allow for
//   stacked calls, such as the plug-in calling back into the host via ARA IPC calls from
//   allocateRenderResourcesAndReturnError:, the call must be dispatched off the main thread, and
//   the main thread must keep spinning the run loop in private mode until the call returns.


// host side: runloop sync init helper to deal with hosts that potentially creating the
// proxy plug-in from some higher level of the run loop
#if !ARA_AUDIOUNITV3_IPC_PROXY_HOST_ONLY
int _globalRunLoopLevel { 0 };
// \todo see also _runLoopCommonModesObserver - it might be a good idea to only have
//       one global observer, where any number of callback-lambdas can be registered?
CFRunLoopObserverRef _globalRunLoopCommonModesObserver { nullptr };
CFRunLoopMode _runLoopSyncPrivateMode { nullptr };
void ARAIPCAUProxyPlugInPrepareRunLoopSync (CFRunLoopMode _Nonnull runLoopSyncPrivateMode)
{
    ARA_INTERNAL_ASSERT (!_globalRunLoopCommonModesObserver);
    _globalRunLoopCommonModesObserver = CFRunLoopObserverCreateWithHandler (kCFAllocatorDefault, kCFRunLoopEntry | kCFRunLoopExit, YES, 0,
        ^void (CFRunLoopObserverRef /*observer*/, CFRunLoopActivity activity)
        {
            switch (activity)
            {
                case kCFRunLoopEntry:
                {
                    ++_globalRunLoopLevel;
                    break;
                }
                case kCFRunLoopExit:
                {
                    --_globalRunLoopLevel;
                    break;
                }
                default:
                {
                    ARA_INTERNAL_ASSERT (false && "unexpected run loop activity");
                    break;
                }
            }
        });
    CFRunLoopAddObserver (CFRunLoopGetMain (), _globalRunLoopCommonModesObserver, kCFRunLoopCommonModes);

    _runLoopSyncPrivateMode = static_cast<CFRunLoopMode> (CFRetain (runLoopSyncPrivateMode));
}
#endif


// run loop sync implementation helper for both proxy plug-in and host
class RunLoopSyncImpl : public FunctionDispatcher
{
public:

    RunLoopSyncImpl ()
    {
        ARA_INTERNAL_ASSERT (CFRunLoopGetCurrent () == CFRunLoopGetMain ());    
    }

    ~RunLoopSyncImpl ()
    {
        if (_privateRunLoopMode)
        {
            if (_runLoopPrivateModeObserver)
            {
                CFRunLoopRemoveObserver (CFRunLoopGetMain (), _runLoopPrivateModeObserver, _privateRunLoopMode);
                CFRelease (_runLoopPrivateModeObserver);
            }

            CFRelease (_privateRunLoopMode);
        }

        if (_runLoopCommonModesObserver)
        {
            CFRunLoopRemoveObserver (CFRunLoopGetMain (), _runLoopCommonModesObserver, kCFRunLoopCommonModes);
            CFRelease (_runLoopCommonModesObserver);
        }
    }

    void setRunLoopMessageChannel (NSObject<AUMessageChannel> * messageChannel)
    {
        _messageChannel = messageChannel;
    }

    // called from the host side:
    // This side is leading the sync and will run normally, while the other side will be switched
    // into follower mode and will wait in private mode whenever exiting its lowest level of
    // common mode run loop until the leader wakes it up (which it must do regularly).
    void initRunLoopSyncAsLeader (CFRunLoopMode privateRunLoopMode, int globalRunLoopLevel)
    {
        ARA_RL_SYNC_LOG ("initializing run loop leader with private run loop mode %s", ARA_RL_MODE_AS_STRING (privateRunLoopMode));

        ARA_INTERNAL_ASSERT (privateRunLoopMode && (CFStringGetLength (privateRunLoopMode) != 0));
        ARA_INTERNAL_ASSERT (_messageChannel);

        _setPrivateRunLoopMode (privateRunLoopMode);
        _runLoopRecursionLevel = globalRunLoopLevel;
        _lastRemoteLoopRecursionLevel = -1;

        _createRunLoopObservers ();

        // run an initial sync cycle which will switch the other side into follower mode
        _spinRemoteRunLoopInMode (privateRunLoopMode);
    }

    bool isInitialized ()
    {
        return _privateRunLoopMode != nullptr;
    }

    bool filterRunLoopSyncMessages (NSDictionary * message)
    {
        // check if this is a runloop sync message
        const auto mode { (__bridge CFRunLoopMode) [message objectForKey:@"rlsync"]};
        if (!mode)
            return false;

        //ARA_RL_SYNC_LOG ("dispatches received remote run loop mode %s to creation thread", ARA_RL_MODE_AS_STRING (mode));
        CFRetain (mode);
        dispatchToCreationThread ([this, /*CF_CONSUMED*/ mode]
            {
                ARA_INTERNAL_ASSERT (CFRunLoopGetCurrent () == CFRunLoopGetMain ());

                // if we receive the first message without run loop observation enabled,
                // we'll instead init in follower mode after the current run loop cycle, see initRunLoopSyncAsLeader ()
                if (!_runLoopCommonModesObserver)
                {
                    _initRunLoopSyncAsFollower (mode);
                    CFRelease (mode);
                    return;
                }

                // after initialization, we shall only receive rl sync messages while waiting in private mode,
                // after having sent the return mode message to the other side
                ARA_INTERNAL_ASSERT (_waitInPrivateMode);
                ARA_INTERNAL_ASSERT (_pendingReturnMode == nullptr);

                // we should never receive the private mode as target mode
                ARA_INTERNAL_ASSERT (!CFEqual (mode, _privateRunLoopMode));

                // if the received mode is empty, the other side simply did run in the mode we requested,
                // otherwise it did run the loop recursively, which we need to pick up when exiting private wait
                ARA_INTERNAL_ASSERT (_pendingStackedMode == nullptr);
                if (CFStringGetLength (mode) != 0)
                {
                    ARA_RL_SYNC_LOG ("will exit private mode to run stacked mode %s", ARA_RL_MODE_AS_STRING (mode));
                    _pendingStackedMode = mode;         // consumes the retain
                }
                else
                {
                    ARA_RL_SYNC_LOG ("will exit private mode to continue previous mode");
                    CFRelease (mode);
                }
                _waitInPrivateMode = false;
                CFRunLoopStop (CFRunLoopGetMain ());
            });

        return true;
    }

    bool waitForMessageOnCurrentThread (CFTimeInterval timeout)
    {
        ARA_INTERNAL_ASSERT (CFRunLoopGetCurrent () == CFRunLoopGetMain ());
        ARA_INTERNAL_ASSERT (isInitialized ());
        ARA_RL_SYNC_LOG ("will spin run loop while waiting for ARA message in private mode %s", ARA_RL_MODE_AS_STRING (_privateRunLoopMode));
        const auto result { CFRunLoopRunInMode (_privateRunLoopMode, timeout, YES) };
        ARA_RL_SYNC_LOG ("did spin run loop while waiting for ARA message in private mode %s", ARA_RL_MODE_AS_STRING (_privateRunLoopMode));
        return result != kCFRunLoopRunTimedOut;
    }

private:

    // setup on the plug-in side, triggered when receiving the initial run loop sync message from the host
    void _initRunLoopSyncAsFollower (CFRunLoopMode privateRunLoopMode)
    {
        ARA_INTERNAL_ASSERT (_messageChannel);
#if ARA_ENABLE_INTERNAL_ASSERTS
        const auto currentMode { CFRunLoopCopyCurrentMode (CFRunLoopGetCurrent ()) };
        ARA_INTERNAL_ASSERT (CFEqual (currentMode, kCFRunLoopDefaultMode));
        CFRelease (currentMode);
#endif

        ARA_RL_SYNC_LOG ("initializing run loop follower with private run loop mode %s", ARA_RL_MODE_AS_STRING (privateRunLoopMode));

        _setPrivateRunLoopMode (privateRunLoopMode);

        _createRunLoopObservers ();
        _isFollower = true;
        _runLoopRecursionLevel = 1;
        _lastRemoteLoopRecursionLevel = 0;

        // we will enter wait state from run loop observer when exiting here
        CFRunLoopStop (CFRunLoopGetMain ());
    }

    // internal setup helper for both sides
    void _setPrivateRunLoopMode (CFRunLoopMode privateRunLoopMode)
    {
        ARA_INTERNAL_ASSERT (CFRunLoopGetCurrent () == CFRunLoopGetMain ());
        ARA_INTERNAL_ASSERT (privateRunLoopMode != nullptr);
        ARA_INTERNAL_ASSERT (_privateRunLoopMode == nullptr);
        _privateRunLoopMode = static_cast<CFRunLoopMode> (CFRetain (privateRunLoopMode));
        addRunLoopMode (_privateRunLoopMode);
    }

    // run loop observation
    void _createRunLoopObservers ()
    {
        ARA_INTERNAL_ASSERT (CFRunLoopGetCurrent () == CFRunLoopGetMain ());
        ARA_INTERNAL_ASSERT (!_runLoopCommonModesObserver);
        ARA_INTERNAL_ASSERT (!_runLoopPrivateModeObserver);
        ARA_INTERNAL_ASSERT (_privateRunLoopMode);

        const auto commonActivities { kCFRunLoopEntry | kCFRunLoopBeforeWaiting | kCFRunLoopExit };
        _runLoopCommonModesObserver = CFRunLoopObserverCreateWithHandler (kCFAllocatorDefault, commonActivities, YES, 0,
                        ^void ([[maybe_unused]] CFRunLoopObserverRef o, CFRunLoopActivity activity)
                        {
#if ARA_ENABLE_DEBUG_OUTPUT
                            [[maybe_unused]] auto _this { this };   // just to be able to see this in the Xcode debugger's local variables

                            const auto dbgCurrentMode { CFRunLoopCopyCurrentMode (CFRunLoopGetCurrent ()) };
                            ARA_INTERNAL_ASSERT (dbgCurrentMode);
                            ARA_RL_SYNC_LOG ("observer %p triggered %s at recursion level %i(%i), mode %s", o,
                                                (activity == kCFRunLoopEntry) ? "kCFRunLoopEntry" :
                                                (activity == kCFRunLoopExit) ? "kCFRunLoopExit" : "kCFRunLoopBeforeWaiting",
                                                _runLoopRecursionLevel, _lastRemoteLoopRecursionLevel,
                                                ARA_RL_MODE_AS_STRING (dbgCurrentMode));
                            CFRelease (dbgCurrentMode);
#endif

                            if (activity == kCFRunLoopEntry)
                            {
                                ++_runLoopRecursionLevel;
                                return;
                            }

                            const bool isExit { activity == kCFRunLoopExit };
                            ARA_INTERNAL_ASSERT (isExit || (activity == kCFRunLoopBeforeWaiting));

                            const auto level { _runLoopRecursionLevel };
                            if (isExit)
                                --_runLoopRecursionLevel;

                            const auto currentMode { CFRunLoopCopyCurrentMode (CFRunLoopGetCurrent ()) };

                            // don't sync if running modally
                            // \todo ViewBridge does some special handling of modal mode that does not work with our run loop sync,
                            //       so for now we just keep either side running while in modal mode.
                            // \todo for some reason, when the plug-in calls NSApplication -runModal from NSEventTrackingRunLoopMode,
                            //       macOS 15.7 will send a few events in NSEventTrackingRunLoopMode before switching to NSModalPanelRunLoopMode
                            //       we currently work around by checking whether the NSApplication has a modalWindow...
                            if (CFEqual (currentMode, (__bridge CFRunLoopMode) NSModalPanelRunLoopMode) || [NSApp modalWindow])
                            {
                                CFRelease (currentMode);
                                return;
                            }

                            // if we're running in a stacked mode, let the other side run stacked as well
                            if (_lastRemoteLoopRecursionLevel + 1 != level)
                            {
                                _spinRemoteRunLoopInMode (currentMode);         // consumes the mode
                                return;
                            }

                            // special case on the follower side, see design overview above:
                            // we can never actually leave the lowest run loop level, instead we
                            // must return to private mode upon exit, and stop instead of waiting
                            if (_isFollower && (level == 1))
                            {
                                if (CFEqual (currentMode, kCFRunLoopDefaultMode))
                                {
                                    if (isExit)
                                        _spinRemoteRunLoopInMode (CFSTR(""));   // yield control to leader
                                    else
                                        // \todo how to handle this event properly?
                                        //       can "forwarding" it to kCFRunLoopExit via CFRunLoopStop() break something?
                                        //       do we need to additionally check the mode?
                                        //       or is it even generally better to always stop the loop and return control to the leader?
                                        CFRunLoopStop (CFRunLoopGetMain ());
                                }
                                else
                                {
                                    _spinRemoteRunLoopInMode (currentMode);     // consumes the mode
                                    return;
                                }
                            }

                            CFRelease (currentMode);
                        });
        CFRunLoopAddObserver (CFRunLoopGetMain (), _runLoopCommonModesObserver, kCFRunLoopCommonModes);

        const auto privateActivities { kCFRunLoopEntry };
        _runLoopPrivateModeObserver = CFRunLoopObserverCreateWithHandler (kCFAllocatorDefault, privateActivities, YES, 0,
                        ^void ([[maybe_unused]] CFRunLoopObserverRef o, CFRunLoopActivity activity)
                        {
                            switch (activity)
                            {
                                case kCFRunLoopEntry:
                                {
                                    if (_pendingReturnMode)
                                    {
                                        _sendRLSyncMessage (_pendingReturnMode);
                                        CFRelease (_pendingReturnMode);
                                        _pendingReturnMode = nullptr;
                                    }
                                    break;
                                }
                                default:
                                {
                                    ARA_INTERNAL_ASSERT (false && "unexpected run loop activity");
                                    break;
                                }
                            }
                        });
        CFRunLoopAddObserver (CFRunLoopGetMain (), _runLoopPrivateModeObserver, _privateRunLoopMode);
    }

    void _spinRemoteRunLoopInMode (CF_CONSUMED CFRunLoopMode mode)
    {
#if ARA_ENABLE_INTERNAL_ASSERTS
        ARA_INTERNAL_ASSERT (CFRunLoopGetCurrent () == CFRunLoopGetMain ());
        ARA_INTERNAL_ASSERT (isInitialized ());
        const auto currentMode { CFRunLoopCopyCurrentMode (CFRunLoopGetCurrent ()) };
        ARA_INTERNAL_ASSERT (!currentMode /*happens at startup*/ || !CFEqual (currentMode, _privateRunLoopMode));
        if (currentMode)
            CFRelease (currentMode);
        ARA_INTERNAL_ASSERT (!_waitInPrivateMode);
#endif

        while (true)                        // retry loop for stacked operation
        {
            ARA_RL_SYNC_LOG ("will yield control to remote run loop in mode %s, level %i", ARA_RL_MODE_AS_STRING (mode), _runLoopRecursionLevel);
            ARA_INTERNAL_ASSERT (_pendingStackedMode == nullptr);
            ARA_INTERNAL_ASSERT (_pendingReturnMode == nullptr);
            _pendingReturnMode = mode;      // _runLoopPrivateModeObserver will send this when we enter the private mode for the first time
            _waitInPrivateMode = true;
            do
            {
                [[maybe_unused]] const auto result { CFRunLoopRunInMode (_privateRunLoopMode, 0.1, YES) };
                ARA_RL_SYNC_LOG ("did spin private run loop mode, %s", (result == kCFRunLoopRunTimedOut) ? "timing out" : "handling event");
            }
            while (_waitInPrivateMode);

            if (_pendingStackedMode)
            {
                const auto pendingMode { _pendingStackedMode }; // local variable consumes the retain
                _pendingStackedMode = nullptr;
                ARA_RL_SYNC_LOG ("did regain control from remote run loop, will run stacked mode %s, level %i->%i",
                                                    ARA_RL_MODE_AS_STRING (pendingMode), _runLoopRecursionLevel, _runLoopRecursionLevel + 1);

                // when reaching the lowest level on the follower side and asked to run in default mode,
                // instead of spinning a new loop iteration we exit the private run loop mode so that
                // the underlying cycle will spin once, after which we return to private mode from our observer
                if (_isFollower && (_runLoopRecursionLevel == 0) && CFEqual (pendingMode, kCFRunLoopDefaultMode))
                {
                    CFRelease (pendingMode);
                    CFRunLoopWakeUp (CFRunLoopGetCurrent ());   // ensure the default run loop wakes up immediately after exiting the private mode
                    // will send handshake and return to waiting from rl observer
                    break;
                }
                else
                {
                    ARA_RL_SYNC_LOG ("will run stacked in mode %s, level %i->%i", ARA_RL_MODE_AS_STRING (pendingMode),  _runLoopRecursionLevel, _runLoopRecursionLevel + 1);

                    const auto prevLastRemoteLoopRecursionLevel { _lastRemoteLoopRecursionLevel };
                    _lastRemoteLoopRecursionLevel = _runLoopRecursionLevel;

                    while (CFRunLoopRunInMode (pendingMode, 0.0, YES) == kCFRunLoopRunHandledSource)
                    {};
                    CFRelease (pendingMode);

                    _lastRemoteLoopRecursionLevel = prevLastRemoteLoopRecursionLevel;
                    ARA_RL_SYNC_LOG ("did run stacked in mode %s, level %i->%i", ARA_RL_MODE_AS_STRING (mode),  _runLoopRecursionLevel +1, _runLoopRecursionLevel);

                    // wait again after returning control to the other side
                    mode = CFSTR("");
                    CFRetain (mode);
                }
            }
            else
            {
                ARA_RL_SYNC_LOG ("did regain control from remote run loop to continue previous mode at level %i", _runLoopRecursionLevel);
                break;
            }
        }
    }

    void _sendRLSyncMessage (CFRunLoopMode mode)
    {
        ARA_RL_SYNC_LOG ("sending sync message for mode %s", ARA_RL_MODE_AS_STRING (mode));
        const auto message { [NSDictionary dictionaryWithObject:(__bridge NSString *) mode forKey:@"rlsync"] };
        [[maybe_unused]] NSDictionary * reply;
        if (_isFollower)
            reply = _messageChannel.callHostBlock (message);
        else
            reply = [_messageChannel callAudioUnit:message];
        ARA_INTERNAL_ASSERT (reply.count == 0);
    }

private:
    NSObject<AUMessageChannel> * _messageChannel;
    CFRunLoopMode _privateRunLoopMode { nullptr };
    CFRunLoopObserverRef _runLoopCommonModesObserver { nullptr };   // \todo when using multiple plugins in the host,
    CFRunLoopObserverRef _runLoopPrivateModeObserver { nullptr };   //       we should consider sharing these observer
    int _runLoopRecursionLevel { 1 };
    int _lastRemoteLoopRecursionLevel { -1 };
    bool _isFollower { false };
    bool _waitInPrivateMode { false };
    CFRunLoopMode _pendingStackedMode { nullptr };      // evaluated when exiting private mode \todo we should be able to reuse _pendingReturnMode
    CFRunLoopMode _pendingReturnMode { nullptr };       // evaluated when entering private mode
};


// message channel base class for both proxy implementations
class AudioUnitMessageChannel : public MessageChannel
{
public:
    AudioUnitMessageChannel (NSObject<AUMessageChannel> * _Nonnull audioUnitChannel, RunLoopSyncImpl * runLoopSyncImpl)
    : _audioUnitChannel { audioUnitChannel },
      _runLoopSyncImpl { runLoopSyncImpl }
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

    NSObject<AUMessageChannel> * getAudioUnitChannel ()
    {
        return _audioUnitChannel;
    }

    void routeReceivedDictionary (NSDictionary * _Nonnull dictionary)
    {
        if (_runLoopSyncImpl && _runLoopSyncImpl->filterRunLoopSyncMessages (dictionary))
            return;

        _currentRoutingThread = [NSThread currentThread];
        ARA_INTERNAL_ASSERT (![NSThread isMainThread]);
        ARA_INTERNAL_ASSERT (dictionary.allKeys.count == 1);
        ARA_INTERNAL_ASSERT (dictionary.allValues.count == 1);
        ARA_INTERNAL_ASSERT ([@"msg" isEqualTo: dictionary.allKeys.firstObject]);
        const auto data { (__bridge CFDataRef) dictionary.allValues.firstObject };
        ARA_INTERNAL_ASSERT (data != nullptr);
        auto message { CFIncomingMessage::createWithMessageData (data, true) };
        routeReceivedMessage (std::move (message));
        _currentRoutingThread = nil;
    }

    void sendMessage (std::unique_ptr<OutgoingMessage>&& message) override
    {
        const auto data { message->convertToEncodedData () };
#if !__has_feature(objc_arc)
        auto nsData { (__bridge NSData *) data };
#else
        auto nsData { (__bridge_transfer NSData *) data };
#endif
        auto dictionary { [NSDictionary dictionaryWithObject:nsData forKey:@"msg"] };
        [[maybe_unused]] const auto reply { _sendMessage (dictionary) };
        ARA_INTERNAL_ASSERT ([reply count] == 0);
#if !__has_feature(objc_arc)
        [reply release];
        CFRelease (data);
#endif
    }

    bool receivesMessagesOnCurrentThread () override
    {
        return _currentRoutingThread == [NSThread currentThread];
    }

    bool waitForMessageOnCurrentThread (ARATimeDuration timeout) override
    {
        if (_runLoopSyncImpl && _runLoopSyncImpl->isInitialized ())
            return _runLoopSyncImpl->waitForMessageOnCurrentThread (timeout);
        else
            return CFRunLoopRunInMode (kCFRunLoopDefaultMode, timeout, YES) != kCFRunLoopRunTimedOut;
    }

protected:
    virtual NSDictionary * _sendMessage (NSDictionary * message) = 0;

protected:
    NSObject<AUMessageChannel> * __strong _Nonnull _audioUnitChannel;
    NSThread * __unsafe_unretained _Nullable _currentRoutingThread { nil }; // unretained is fine, only points to [NSThread currentThread]
                                                                            // while on that thread and never dereferenced
    RunLoopSyncImpl * _runLoopSyncImpl;
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
            const auto reply { callHostBlock (message) };
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
    ProxyPlugInMessageChannel (NSObject<AUMessageChannel> * _Nonnull audioUnitChannel, RunLoopSyncImpl * runLoopSyncImpl)
    : AudioUnitMessageChannel { audioUnitChannel, runLoopSyncImpl }
    {
        _audioUnitChannel.callHostBlock =
            ^NSDictionary * _Nullable (NSDictionary * _Nonnull dictionary)
            {
                routeReceivedDictionary (dictionary);
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
class AUProxyPlugIn : public ProxyPlugIn, public RunLoopSyncImpl
{
public:
    static AUProxyPlugIn * createWithAudioUnit (AUAudioUnit * _Nonnull audioUnit)
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
                                   audioUnit };
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
                   AUAudioUnit * _Nonnull initAU)
    : ProxyPlugIn { std::make_unique<Connection> (&CFOutgoingMessage::create,
                                                  ProxyPlugIn::handleReceivedMessage,
                                                  [this] (auto&& func) { dispatchToCreationThread (func); },
                                                  true) },  // \todo shouldn't the AUMessageChannel provide this information?
      RunLoopSyncImpl {},
      _initAU { initAU }
    {
        setRunLoopMessageChannel (mainChannel);
        getConnection ()->setMainThreadChannel (std::make_unique<ProxyPlugInMessageChannel> (mainChannel, this));
        getConnection ()->setOtherThreadsChannel (std::make_unique<ProxyPlugInMessageChannel> (otherChannel, this));
#if !__has_feature(objc_arc)
        [_initAU retain];
#endif

        if (_runLoopSyncPrivateMode)
        {
            initRunLoopSyncAsLeader (_runLoopSyncPrivateMode, _globalRunLoopLevel);

            // cleanup init helpers
            ARA_INTERNAL_ASSERT (_globalRunLoopCommonModesObserver);
            CFRunLoopRemoveObserver (CFRunLoopGetMain (), _globalRunLoopCommonModesObserver, kCFRunLoopCommonModes);
            _globalRunLoopCommonModesObserver = nullptr;

            CFRelease (_runLoopSyncPrivateMode);
            _runLoopSyncPrivateMode = nullptr;
        }
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

ARAIPCProxyPlugInRef ARA_CALL ARAIPCAUProxyPlugInInitialize (AUAudioUnit * _Nonnull audioUnit)
{
    return toIPCRef (AUProxyPlugIn::createWithAudioUnit (audioUnit));
}

void ARA_CALL ARAIPCAUProxyPlugInPerformPendingMainThreadTasks (ARAIPCProxyPlugInRef _Nonnull proxyPlugInRef)
{
    ARA_INTERNAL_ASSERT ([NSThread isMainThread]);
    fromIPCRef (proxyPlugInRef)->getConnection ()->processPendingMessageOnCreationThreadIfNeeded (0.0);
}

void ARA_CALL ARAIPCAUProxyPlugInCallAudioUnitAllowingCallbacks(ARAIPCProxyPlugInRef _Nonnull proxyPlugInRef,
                                                                void (^ _Nonnull audioUnitCall)(void))
{
    if ([NSThread isMainThread])
    {
        std::atomic<bool> ready { false };
        __block const auto readyPtr { &ready };
        dispatch_async (dispatch_get_global_queue (QOS_CLASS_USER_INTERACTIVE, 0),
            ^{
                audioUnitCall ();
                readyPtr->store (true, std::memory_order_release);
            });

        while (!ready.load (std::memory_order_acquire))
            fromIPCRef (proxyPlugInRef)->getConnection ()->processPendingMessageOnCreationThreadIfNeeded (0.001);
    }
    else
    {
        auto semaphore { dispatch_semaphore_create (0) };
        dispatch_async (dispatch_get_global_queue (QOS_CLASS_USER_INTERACTIVE, 0),
            ^{
                audioUnitCall ();
                dispatch_semaphore_signal (semaphore);
            });
        dispatch_semaphore_wait (semaphore, DISPATCH_TIME_FOREVER);
#if !__has_feature(objc_arc)
        dispatch_release (semaphore);
#endif
    }
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

class AUProxyHost : public ProxyHost, public RunLoopSyncImpl
{
public:
    AUProxyHost ()
    : ProxyHost { std::make_unique<Connection> (&CFOutgoingMessage::create,
                                                [this] (auto&& ...args) { handleReceivedMessage (args...); },
                                                [this] (auto&& func) { dispatchToCreationThread (func); },
                                                true) },    // \todo shouldn't the AUMessageChannel provide this information?
      RunLoopSyncImpl {},
      _dispatchSyncSemaphore { dispatch_semaphore_create (0) }
    {
        ARAIPCProxyHostSetBindingHandler (handleBinding);
    }

#if !__has_feature(objc_arc)
    ~AUProxyHost ()
    {
        dispatch_release (_dispatchSyncSemaphore);
    }
#endif

    void setMessageChannel (std::unique_ptr<ProxyHostMessageChannel>&& messageChannel, bool isMainThreadChannel)
    {
        if (isMainThreadChannel)
        {
            setRunLoopMessageChannel (messageChannel->getAudioUnitChannel ());
            getConnection ()->setMainThreadChannel (std::move (messageChannel));
        }
        else
        {
            getConnection ()->setOtherThreadsChannel (std::move (messageChannel));
        }
    }

    void dispatchSyncToMainThread (void (^_Nonnull block) ())
    {
        dispatchToCreationThread ([block, this] ()
                            {
                                block ();
                                dispatch_semaphore_signal (_dispatchSyncSemaphore);
                            });
        dispatch_semaphore_wait (_dispatchSyncSemaphore, DISPATCH_TIME_FOREVER);
    }

private:
    static const ARAPlugInExtensionInstance * ARA_CALL handleBinding (ARAIPCPlugInInstanceRef plugInInstanceRef,
                                                                      ARADocumentControllerRef controllerRef,
                                                                      ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles)
    {
        auto audioUnit { (__bridge AUAudioUnit<ARAAudioUnit> *) plugInInstanceRef };
        return [audioUnit bindToDocumentController:controllerRef withRoles:assignedRoles knownRoles:knownRoles];
    }

private:
    dispatch_semaphore_t _dispatchSyncSemaphore;
};

std::atomic<AUProxyHost*> _proxyHost { nullptr };


void ARA_CALL ARAIPCAUProxyHostDispatchSyncToMainThread (void (^_Nonnull block) ())
{
    if ([NSThread isMainThread])
    {
        block ();
    }
    else if (auto proxyHost { _proxyHost.load (std::memory_order_acquire) })
    {
        proxyHost->dispatchSyncToMainThread (block);
    }
    else
    {
        dispatch_sync (dispatch_get_main_queue (), block);
    }
}


typedef ARA_IPC_REF(ARAIPCMessageChannelRef);
ARA_MAP_IPC_REF (AudioUnitMessageChannel, ARAIPCMessageChannelRef)

static ARAIPCMessageChannelRef _Nullable initializeMessageChannel (NSObject<AUMessageChannel> * _Nonnull audioUnitChannel,
                                                                   bool isMainThreadChannel)
{
    // \todo the connection currently stores the creation thread as main thread for Windows compatibility,
    //       so we need to make sure the proxy is created on the main thread
    ARAIPCAUProxyHostDispatchSyncToMainThread (^void ()
            {
                if (!_proxyHost.load (std::memory_order_relaxed))
                    _proxyHost.store (new AUProxyHost {}, std::memory_order_release);
            });

    auto proxyHost { _proxyHost.load (std::memory_order_relaxed) };
    auto messageChannel { std::make_unique<ProxyHostMessageChannel> (audioUnitChannel, proxyHost) };
    const auto result { toIPCRef (messageChannel.get ()) };
    proxyHost->setMessageChannel (std::move (messageChannel), isMainThreadChannel);

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
            Block_release ((__bridge void *) oldBlock);
        if (newBlock)
            newBlock = (__bridge CallHostBlock) Block_copy ((__bridge void *) newBlock);
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
    ARA_INTERNAL_ASSERT (!_mainMessageChannel && !_otherMessageChannel);

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
