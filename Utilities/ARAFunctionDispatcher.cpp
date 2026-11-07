//------------------------------------------------------------------------------
//! \file       ARAFunctionDispatcher.cpp
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

#include "ARAFunctionDispatcher.h"
#include "ARA_Library/Debug/ARADebug.h"
#include "ARA_Library/Utilities/ARAStdVectorUtilities.h"

namespace ARA {

#if defined (_WIN32)

// from https://devblogs.microsoft.com/oldnewthing/20141015-00/?p=43843
BOOL ConvertToRealHandle(HANDLE h,
                         BOOL bInheritHandle,
                         HANDLE *phConverted)
{
 return DuplicateHandle(GetCurrentProcess(), h,
                        GetCurrentProcess(), phConverted,
                        0, bInheritHandle, DUPLICATE_SAME_ACCESS);
}

HANDLE _GetRealCurrentThread ()
{
    HANDLE currentThread {};
    [[maybe_unused]] const auto success { ConvertToRealHandle (::GetCurrentThread (), FALSE, &currentThread) };
    ARA_INTERNAL_ASSERT (success);
    return currentThread;
}

void APCRouteNewTransactionFunc (ULONG_PTR parameter)
{
    auto funcPtr { reinterpret_cast<FunctionDispatcher::DispatchableFunction*> (parameter) };
    (*funcPtr) ();
    delete funcPtr;
}

#elif defined (__APPLE__)

void FunctionDispatcher::addRunLoopMode (CFRunLoopMode mode)
{
    ARA_INTERNAL_ASSERT (!contains (_runLoopModes, mode));
    CFRetain (mode);
    _runLoopModes.emplace_back (mode);
    CFRunLoopAddSource (_creationThreadRunLoop, _runLoopSource, mode);
}

void FunctionDispatcher::_performRunLoopSource (void* info)
{
    auto dispatcher { reinterpret_cast<FunctionDispatcher*> (info) };
    dispatcher->_mutex.lock ();
    while (!dispatcher->_queue.empty ())
    {
        auto func { dispatcher->_queue.front () };
        dispatcher->_queue.pop ();
        dispatcher->_mutex.unlock ();
 
        func ();
 
        dispatcher->_mutex.lock ();
    }
    dispatcher->_mutex.unlock ();
}

#endif


FunctionDispatcher::FunctionDispatcher ()
:
#if defined (_WIN32)
  _creationThreadHandle { _GetRealCurrentThread () }
{}
#elif defined (__APPLE__)
  _creationThreadRunLoop { CFRunLoopGetCurrent () }
{
    CFRunLoopSourceContext context { 0, this, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, _performRunLoopSource };
    _runLoopSource = CFRunLoopSourceCreate (kCFAllocatorDefault, 0, &context);
    addRunLoopMode (kCFRunLoopCommonModes);
}
#endif

FunctionDispatcher::~FunctionDispatcher ()
{
#if defined (__APPLE__)
    for (auto mode : _runLoopModes)
    {
        CFRunLoopRemoveSource (_creationThreadRunLoop, _runLoopSource, mode);
        CFRelease (mode);
    }
    CFRelease (_runLoopSource);
#endif
}

void FunctionDispatcher::dispatchToCreationThread (DispatchableFunction func)
{
#if defined (_WIN32)
    ARA_INTERNAL_ASSERT (_GetRealCurrentThread () != _creationThreadHandle);
    auto funcPtr { new DispatchableFunction { func } };
    [[maybe_unused]] const auto result { ::QueueUserAPC (APCRouteNewTransactionFunc, _creationThreadHandle, reinterpret_cast<ULONG_PTR> (funcPtr)) };
    ARA_INTERNAL_ASSERT (result != 0);
#elif defined (__APPLE__)
    ARA_INTERNAL_ASSERT (CFRunLoopGetCurrent () != _creationThreadRunLoop);
    _mutex.lock ();
    _queue.emplace (std::move (func));
    _mutex.unlock ();
    CFRunLoopSourceSignal (_runLoopSource);
    CFRunLoopWakeUp (_creationThreadRunLoop);
#endif
}

}   // namespace ARA
