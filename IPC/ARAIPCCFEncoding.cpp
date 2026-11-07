//------------------------------------------------------------------------------
//! \file       ARAIPCCFEncoding.cpp
//!             Implementation of ARAIPCMessageEn-/Decoder backed by CF(Mutable)Array
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

#include "ARAIPCCFEncoding.h"


#if ARA_ENABLE_IPC && defined (__APPLE__)


#include "ARA_Library/Debug/ARADebug.h"

#include <map>
#include <set>


namespace ARA {
namespace IPC {


#if defined (__GNUC__)
    _Pragma ("GCC diagnostic push")
    _Pragma ("GCC diagnostic ignored \"-Wold-style-cast\"")
#endif


CFMessageEncoder::CFMessageEncoder ()
: _array { CFArrayCreateMutable (kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks) }
{}

CFMessageEncoder::~CFMessageEncoder ()
{
    ARA_INTERNAL_ASSERT (!_array);      // should have been reset via convertToEncodedData()/-Array()
    if (_array)
        CFRelease (_array);
}

void CFMessageEncoder::appendInt32 (int32_t argValue)
{
    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberSInt32Type, &argValue) };
    CFArrayAppendValue (_array, argObject);
    CFRelease (argObject);
}

void CFMessageEncoder::appendInt64 (int64_t argValue)
{
    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberSInt64Type, &argValue) };
    CFArrayAppendValue (_array, argObject);
    CFRelease (argObject);
}

void CFMessageEncoder::appendSize (size_t argValue)
{
    static_assert (sizeof (SInt64) == sizeof (size_t), "integer type needs adjustment for this compiler setup");

    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberSInt64Type, &argValue) };
    CFArrayAppendValue (_array, argObject);
    CFRelease (argObject);
}

void CFMessageEncoder::appendFloat (float argValue)
{
    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberFloatType, &argValue) };
    CFArrayAppendValue (_array, argObject);
    CFRelease (argObject);
}

void CFMessageEncoder::appendDouble (double argValue)
{
    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberDoubleType, &argValue) };
    CFArrayAppendValue (_array, argObject);
    CFRelease (argObject);
}

void CFMessageEncoder::appendBytes (const uint8_t* argValue, size_t argSize, bool copy)
{
    ARA_INTERNAL_ASSERT ((argValue != nullptr) || (argSize == 0));
    CFDataRef argObject;
    if (copy)
        argObject = CFDataCreate (kCFAllocatorDefault, argValue, (CFIndex) argSize);
    else
        argObject = CFDataCreateWithBytesNoCopy (kCFAllocatorDefault, argValue, (CFIndex) argSize, kCFAllocatorNull);
    CFArrayAppendValue (_array, argObject);
    CFRelease (argObject);
}

__attribute__((cf_returns_retained)) CFArrayRef CFMessageEncoder::convertToEncodedArray ()
{
    ARA_INTERNAL_ASSERT (_array);       // convertToEncodedData()/-Array() must only be called once
    const auto result { _array };
    _array = nullptr;
    return result;
}

__attribute__((cf_returns_retained)) CFDataRef CFMessageEncoder::convertToEncodedData ()
{
    ARA_INTERNAL_ASSERT (_array);       // convertToEncodedData()/-Array() must only be called once
    const auto result { (CFArrayGetCount (_array) != 0) ?
                            CFPropertyListCreateData (kCFAllocatorDefault, _array, kCFPropertyListBinaryFormat_v1_0, 0, nullptr) : nullptr };
    CFRetain (_array);
    _array = nullptr;
    return result;
}


CFMessageDecoder::CFMessageDecoder (CFArrayRef array, bool retain)
: _array { array }
{
    if (array && retain)
        CFRetain (array);
}

CFMessageDecoder::~CFMessageDecoder ()
{
    if (_array)
        CFRelease (_array);
}

bool CFMessageDecoder::readInt32 (int32_t& argValue) const
{
    CFNumberRef number {};
    if (_array)
        number = (CFNumberRef) CFArrayGetValueAtIndex (_array, _index++);
    if (!number)
    {
        argValue = 0;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (number) == CFNumberGetTypeID ());
    CFNumberGetValue (number, kCFNumberSInt32Type, &argValue);
    return true;
}

bool CFMessageDecoder::readInt64 (int64_t& argValue) const
{
    CFNumberRef number {};
    if (_array)
        number = (CFNumberRef) CFArrayGetValueAtIndex (_array, _index++);
    if (!number)
    {
        argValue = 0;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (number) == CFNumberGetTypeID ());
    CFNumberGetValue (number, kCFNumberSInt64Type, &argValue);
    return true;
}

bool CFMessageDecoder::readSize (size_t& argValue) const
{
    static_assert (sizeof (SInt64) == sizeof (size_t), "integer type needs adjustment for this compiler setup");

    CFNumberRef number {};
    if (_array)
        number = (CFNumberRef) CFArrayGetValueAtIndex (_array, _index++);
    if (!number)
    {
        argValue = 0;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (number) == CFNumberGetTypeID ());
    CFNumberGetValue (number, kCFNumberSInt64Type, &argValue);
    return true;
}

bool CFMessageDecoder::readFloat (float& argValue) const
{
    CFNumberRef number {};
    if (_array)
        number = (CFNumberRef) CFArrayGetValueAtIndex (_array, _index++);
    if (!number)
    {
        argValue = 0.0f;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (number) == CFNumberGetTypeID ());
    CFNumberGetValue (number, kCFNumberFloatType, &argValue);
    return true;
}

bool CFMessageDecoder::readDouble (double& argValue) const
{
    CFNumberRef number {};
    if (_array)
        number = (CFNumberRef) CFArrayGetValueAtIndex (_array, _index++);
    if (!number)
    {
        argValue = 0.0;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (number) == CFNumberGetTypeID ());
    CFNumberGetValue (number, kCFNumberDoubleType, &argValue);
    return true;
}

bool CFMessageDecoder::readBytes (const uint8_t*& argValue, size_t& argSize) const
{
    CFDataRef data { nullptr };
    if (_array)
        data = (CFDataRef) CFArrayGetValueAtIndex (_array, _index++);
    if (!data)
    {
        argValue = nullptr;
        argSize = 0;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (data) == CFDataGetTypeID ());
    argSize = (size_t) CFDataGetLength (data);
    argValue = CFDataGetBytePtr (data);
    return true;
}

std::unique_ptr<CFMessageDecoder> CFMessageDecoder::createWithMessageData (CFDataRef messageData)
{
    if (CFDataGetLength (messageData) == 0)
        return nullptr;

    auto array { (CFArrayRef) CFPropertyListCreateWithData (kCFAllocatorDefault, messageData, kCFPropertyListImmutable, nullptr, nullptr) };
    ARA_INTERNAL_ASSERT (array && (CFGetTypeID (array) == CFArrayGetTypeID ()));
    return std::make_unique<CFMessageDecoder> (array, false);
}

#if defined (__APPLE__)
    _Pragma ("GCC diagnostic pop")
#endif

}   // namespace IPC
}   // namespace ARA

#endif // ARA_ENABLE_IPC && defined (__APPLE__)
