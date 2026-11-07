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
#include <string>


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

void CFMessageEncoder::appendString (const char* argValue)
{
    ARA_INTERNAL_ASSERT (argValue);
    auto argObject { CFStringCreateWithCString (kCFAllocatorDefault, argValue, kCFStringEncodingUTF8) };
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

void CFMessageEncoder::appendSubMessage (std::unique_ptr<MessageEncoder>&& argValue)
{
    const auto subMessage (dynamic_cast<CFMessageEncoder*> (argValue.release ()));
    ARA_INTERNAL_ASSERT (subMessage != nullptr);

    CFArrayAppendArray (_array, subMessage->_array, CFRangeMake (0, CFArrayGetCount (subMessage->_array)));
    
    delete subMessage;
}

std::unique_ptr<MessageEncoder> CFMessageEncoder::createEncoder ()
{
    return CFMessageEncoder::create ();
}

__attribute__((cf_returns_retained)) CFArrayRef CFMessageEncoder::copyArray () const
{
    CFRetain (_array);
    return _array;
}

__attribute__((cf_returns_retained)) CFDataRef CFMessageEncoder::createMessageEncoderData ()  const
{
    if (CFArrayGetCount (_array) == 0)
        return nullptr;
    return CFPropertyListCreateData (kCFAllocatorDefault, _array, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
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

bool CFMessageDecoder::readString (const char*& argValue) const
{
    const void* value {};
    if (_array)
        value = CFArrayGetValueAtIndex (_array, _index++);
    if (!value)
    {
        argValue = nullptr;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (value) == CFStringGetTypeID ());
    argValue = CFStringGetCStringPtr ((CFStringRef) value, kCFStringEncodingUTF8);
    if (!argValue)          // CFStringGetCStringPtr() may fail e.g. with chord names like "G/D"
    {
        const auto length { CFStringGetLength ((CFStringRef) value) };
        std::string temp;   // \todo does not work: { static_cast<size_t> (length), char { 0 } };
        temp.assign (static_cast<size_t> (length) , char { 0 });
        [[maybe_unused]] const auto count { CFStringGetBytes ((CFStringRef) value, CFRangeMake (0, length), kCFStringEncodingUTF8, 0, false, (UInt8*)(&temp[0]), length, nullptr) };
        ARA_INTERNAL_ASSERT (count == length);
        static std::set<std::string> strings;   // \todo static cache of "undecodeable" strings requires single-threaded use - maybe make iVar instead?
        strings.insert (temp);
        argValue = strings.find (temp)->c_str ();
    }
    return true;
}

bool CFMessageDecoder::readBytesSize (size_t& argSize) const
{
    ARA_INTERNAL_ASSERT (_bytesCache == nullptr);
    if (_array)
        _bytesCache = (CFDataRef) CFArrayGetValueAtIndex (_array, _index++);
    if (!_bytesCache)
    {
        argSize = 0;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (_bytesCache) == CFDataGetTypeID ());
    argSize = (size_t) CFDataGetLength (_bytesCache);
    return true;
}

void CFMessageDecoder::readBytes (uint8_t* argValue) const
{
    ARA_INTERNAL_ASSERT (_bytesCache != nullptr);
    const auto length { CFDataGetLength (_bytesCache) };
    CFDataGetBytes (_bytesCache, CFRangeMake (0, length), argValue);
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
