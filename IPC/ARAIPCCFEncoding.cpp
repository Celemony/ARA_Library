//------------------------------------------------------------------------------
//! \file       ARAIPCCFEncoding.cpp
//!             Implementation of ARAIPCMessageEn-/Decoder backed by CF(Mutable)Dictionary
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


// helper class to deal with CoreFoundation reference counting
class _CFReleaser
{
public:
    explicit _CFReleaser (CFStringRef ref) : _ref { ref } {}
    _CFReleaser (const _CFReleaser& other) { _ref = (CFStringRef) CFRetain (other._ref); }
    _CFReleaser (_CFReleaser&& other) { _ref = other._ref; other._ref = CFStringRef {}; }
    ~_CFReleaser () { CFRelease (_ref); }
    operator CFStringRef () { return _ref; }
private:
    CFStringRef _ref;
};


// wrap key value into CFString (no reference count transferred to caller)
CFStringRef _getEncodedKey (MessageArgumentKey argKey)
{
    // \todo All plist formats available for CFPropertyListCreateData () in createEncodedMessage () need CFString keys.
    //       Once we switch to the more modern (NS)XPC API we shall be able to use CFNumber keys directly...
    static std::map<MessageArgumentKey, _CFReleaser> cache;
    auto existingEntry { cache.find (argKey) };
    if (existingEntry != cache.end ())
        return existingEntry->second;
    return cache.emplace (argKey, CFStringCreateWithCString (kCFAllocatorDefault, std::to_string (argKey).c_str (), kCFStringEncodingUTF8)).first->second;
}



CFMessageEncoder::CFMessageEncoder ()
: _dictionary { CFDictionaryCreateMutable (kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) }
{}

CFMessageEncoder::~CFMessageEncoder ()
{
    CFRelease (_dictionary);
}

void CFMessageEncoder::appendInt32 (MessageArgumentKey argKey, int32_t argValue)
{
    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberSInt32Type, &argValue) };
    CFDictionarySetValue (_dictionary, _getEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void CFMessageEncoder::appendInt64 (MessageArgumentKey argKey, int64_t argValue)
{
    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberSInt64Type, &argValue) };
    CFDictionarySetValue (_dictionary, _getEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void CFMessageEncoder::appendSize (MessageArgumentKey argKey, size_t argValue)
{
    static_assert (sizeof (SInt64) == sizeof (size_t), "integer type needs adjustment for this compiler setup");

    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberSInt64Type, &argValue) };
    CFDictionarySetValue (_dictionary, _getEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void CFMessageEncoder::appendFloat (MessageArgumentKey argKey, float argValue)
{
    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberFloatType, &argValue) };
    CFDictionarySetValue (_dictionary, _getEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void CFMessageEncoder::appendDouble (MessageArgumentKey argKey, double argValue)
{
    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberDoubleType, &argValue) };
    CFDictionarySetValue (_dictionary, _getEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void CFMessageEncoder::appendString (MessageArgumentKey argKey, const char * argValue)
{
    auto argObject { CFStringCreateWithCString (kCFAllocatorDefault, argValue, kCFStringEncodingUTF8) };
    CFDictionarySetValue (_dictionary, _getEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void CFMessageEncoder::appendBytes (MessageArgumentKey argKey, const uint8_t * argValue, size_t argSize, bool copy)
{
    CFDataRef argObject;
    if (copy)
        argObject = CFDataCreate (kCFAllocatorDefault, argValue, (CFIndex) argSize);
    else
        argObject = CFDataCreateWithBytesNoCopy (kCFAllocatorDefault, argValue, (CFIndex) argSize, kCFAllocatorNull);
    CFDictionarySetValue (_dictionary, _getEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

MessageEncoder* CFMessageEncoder::appendSubMessage (MessageArgumentKey argKey)
{
    auto argObject = new CFMessageEncoder {};
    CFDictionarySetValue (_dictionary, _getEncodedKey (argKey), argObject->_dictionary);
    return argObject;
}

__attribute__((cf_returns_retained)) CFMutableDictionaryRef CFMessageEncoder::copyDictionary () const
{
    CFRetain (_dictionary);
    return _dictionary;
}

__attribute__((cf_returns_retained)) CFDataRef CFMessageEncoder::createMessageEncoderData ()  const
{
    if (CFDictionaryGetCount (_dictionary) == 0)
        return nullptr;
    return CFPropertyListCreateData (kCFAllocatorDefault, _dictionary, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
}


CFMessageDecoder::CFMessageDecoder (CFDictionaryRef dictionary)
: CFMessageDecoder::CFMessageDecoder (dictionary, true)
{}

CFMessageDecoder::CFMessageDecoder (CFDictionaryRef dictionary, bool retain)
: _dictionary { dictionary }
{
    if (dictionary && retain)
        CFRetain (dictionary);
}

CFMessageDecoder::~CFMessageDecoder ()
{
    if (_dictionary)
        CFRelease (_dictionary);
}

bool CFMessageDecoder::readInt32 (MessageArgumentKey argKey, int32_t* argValue) const
{
    CFNumberRef number {};
    if (_dictionary)
        number = (CFNumberRef) CFDictionaryGetValue (_dictionary, _getEncodedKey (argKey));
    if (!number)
    {
        *argValue = 0;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (number) == CFNumberGetTypeID ());
    CFNumberGetValue (number, kCFNumberSInt32Type, argValue);
    return true;
}

bool CFMessageDecoder::readInt64 (MessageArgumentKey argKey, int64_t* argValue) const
{
    CFNumberRef number {};
    if (_dictionary)
        number = (CFNumberRef) CFDictionaryGetValue (_dictionary, _getEncodedKey (argKey));
    if (!number)
    {
        *argValue = 0;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (number) == CFNumberGetTypeID ());
    CFNumberGetValue (number, kCFNumberSInt64Type, argValue);
    return true;
}

bool CFMessageDecoder::readSize (MessageArgumentKey argKey, size_t* argValue) const
{
    static_assert (sizeof (SInt64) == sizeof (size_t), "integer type needs adjustment for this compiler setup");

    CFNumberRef number {};
    if (_dictionary)
        number = (CFNumberRef) CFDictionaryGetValue (_dictionary, _getEncodedKey (argKey));
    if (!number)
    {
        *argValue = 0;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (number) == CFNumberGetTypeID ());
    CFNumberGetValue (number, kCFNumberSInt64Type, argValue);
    return true;
}

bool CFMessageDecoder::readFloat (MessageArgumentKey argKey, float* argValue) const
{
    CFNumberRef number {};
    if (_dictionary)
        number = (CFNumberRef) CFDictionaryGetValue (_dictionary, _getEncodedKey (argKey));
    if (!number)
    {
        *argValue = 0.0f;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (number) == CFNumberGetTypeID ());
    CFNumberGetValue (number, kCFNumberFloatType, argValue);
    return true;
}

bool CFMessageDecoder::readDouble (MessageArgumentKey argKey, double* argValue) const
{
    CFNumberRef number {};
    if (_dictionary)
        number = (CFNumberRef) CFDictionaryGetValue (_dictionary, _getEncodedKey (argKey));
    if (!number)
    {
        *argValue = 0.0;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (number) == CFNumberGetTypeID ());
    CFNumberGetValue (number, kCFNumberDoubleType, argValue);
    return true;
}

bool CFMessageDecoder::readString (MessageArgumentKey argKey, const char ** argValue) const
{
    CFStringRef string {};
    if (_dictionary)
        string = (CFStringRef) CFDictionaryGetValue (_dictionary, _getEncodedKey (argKey));
    if (!string)
    {
        *argValue = nullptr;
        return false;
    }
    ARA_INTERNAL_ASSERT (string && (CFGetTypeID (string) == CFStringGetTypeID ()));
    *argValue = CFStringGetCStringPtr (string, kCFStringEncodingUTF8);
    if (!*argValue)         // CFStringGetCStringPtr() may fail e.g. with chord names like "G/D"
    {
        const auto length { CFStringGetLength (string) };
        std::string temp;   // \todo does not work: { static_cast<size_t> (length), char { 0 } };
        temp.assign ( static_cast<size_t> (length) , char { 0 } );
        CFIndex ARA_MAYBE_UNUSED_VAR (count) { CFStringGetBytes (string, CFRangeMake (0, length), kCFStringEncodingUTF8, 0, false, (UInt8*)(&temp[0]), length, nullptr) };
        ARA_INTERNAL_ASSERT (count == length);
        static std::set<std::string> strings;   // \todo static cache of "undecodeable" strings requires single-threaded use - maybe make iVar instead?
        strings.insert (temp);
        *argValue = strings.find (temp)->c_str ();
    }
    return true;
}

bool CFMessageDecoder::readBytesSize (MessageArgumentKey argKey, size_t* argSize) const
{
    CFDataRef bytes {};
    if (_dictionary)
        bytes = (CFDataRef) CFDictionaryGetValue (_dictionary, _getEncodedKey (argKey));
    if (!bytes)
    {
        *argSize = 0;
        return false;
    }
    ARA_INTERNAL_ASSERT (bytes && (CFGetTypeID (bytes) == CFDataGetTypeID ()));
    *argSize = (size_t) CFDataGetLength (bytes);
    return true;
}

void CFMessageDecoder::readBytes (MessageArgumentKey argKey, uint8_t* argValue) const
{
    ARA_INTERNAL_ASSERT (_dictionary);
    auto bytes { (CFDataRef) CFDictionaryGetValue (_dictionary, _getEncodedKey (argKey)) };
    ARA_INTERNAL_ASSERT (bytes && (CFGetTypeID (bytes) == CFDataGetTypeID ()));
    const auto length { CFDataGetLength (bytes) };
    CFDataGetBytes (bytes, CFRangeMake (0, length), argValue);
}

MessageDecoder* CFMessageDecoder::readSubMessage (MessageArgumentKey argKey) const
{
    auto dictionary { (CFDictionaryRef) CFDictionaryGetValue (_dictionary, _getEncodedKey (argKey)) };
    ARA_INTERNAL_ASSERT (!dictionary || (CFGetTypeID (dictionary) == CFDictionaryGetTypeID ()));
    if (dictionary == nullptr)
        return nullptr;
    return new CFMessageDecoder { dictionary };
}

bool CFMessageDecoder::hasDataForKey (MessageArgumentKey argKey) const
{
    return CFDictionaryContainsKey (_dictionary, _getEncodedKey (argKey));
}

CFMessageDecoder* CFMessageDecoder::createWithMessageData (CFDataRef messageData)
{
    if (CFDataGetLength (messageData) == 0)
        return nullptr;

    auto dictionary { (CFDictionaryRef) CFPropertyListCreateWithData (kCFAllocatorDefault, messageData, kCFPropertyListImmutable, nullptr, nullptr) };
    ARA_INTERNAL_ASSERT (dictionary && (CFGetTypeID (dictionary) == CFDictionaryGetTypeID ()));
    return new CFMessageDecoder { dictionary, false };
}

#if defined (__APPLE__)
    _Pragma ("GCC diagnostic pop")
#endif

}   // namespace IPC
}   // namespace ARA

#endif // ARA_ENABLE_IPC && defined (__APPLE__)
