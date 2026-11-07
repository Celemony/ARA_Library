//------------------------------------------------------------------------------
//! \file       ARAFunctionDispatcher.h
//!             Enable execution of opaque functions on other threads.
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

#ifndef ARAFunctionDispatcher_h
#define ARAFunctionDispatcher_h

#include "ARA_API/ARAInterface.h"

#include <functional>

#if defined (_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#elif defined (__APPLE__)
    #include <CoreFoundation/CFRunLoop.h>

    #include <mutex>
    #include <queue>
    #include <vector>
#endif

namespace ARA {

//! @addtogroup ARA_Library_Utility_FunctionDispatcher
//! @{

//! Class that allows for wrapping code as opaque function and execute it on a different thread.
class FunctionDispatcher
{
public:
    //! Construct each instances on the thread it should dispatch to.
    //! The creation thread must run an appropriate run loop (macOS) or regularly enter alertable state (Windows).
    FunctionDispatcher ();
    ~FunctionDispatcher ();

    //! Opaque block/lambda/function that can be dispatched.
    using DispatchableFunction = std::function<void ()>;

    //! Call this method from any thread other than the creation thread in order to asynchronously
    //! execute the given function on the creation thread.
    //! When dispatching functions from multiple threads, their relative execution order is undefined,
    //! only dispatches from the same thread are guaranteed to be executed in the order they were dispatched.
    void dispatchToCreationThread (DispatchableFunction func);

#if defined (__APPLE__)
    //! Optionally add additional run loop modes in which dispatched functions may be executed.
    void addRunLoopMode (CFRunLoopMode mode);

private:
    static void _performRunLoopSource (void* info);
#endif

private:
#if defined (_WIN32)
    HANDLE const _creationThreadHandle;
#elif defined (__APPLE__)
    CFRunLoopRef const _creationThreadRunLoop;
    CFRunLoopSourceRef _runLoopSource;
    std::vector<CFRunLoopMode> _runLoopModes;
    std::queue<DispatchableFunction> _queue;    // \todo instead of locking, use a lockless concurrent queue,
    std::recursive_mutex _mutex;                // eg this one: https://github.com/hogliux/farbot
#else
    #error "not yet implemented on this platform"
#endif
};

//! @} ARA_Library_Utility_FunctionDispatcher

}   // namespace ARA

#endif // ARAFunctionDispatcher_h
