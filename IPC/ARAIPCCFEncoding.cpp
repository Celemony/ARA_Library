//------------------------------------------------------------------------------
//! \file       ARAIPCCFEncoding.cpp
//!             Implementation of ARAIPCMessageEn-/Decoder backed by CF(Mutable)Dictionary
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
CFStringRef _getEncodedKey (ARAIPCMessageKey argKey)
{
    // \todo All plist formats available for CFPropertyListCreateData () in createEncodedMessage () need CFString keys.
    //       Once we switch to the more modern (NS)XPC API we shall be able to use CFNumber keys directly...
    static std::map<ARAIPCMessageKey, _CFReleaser> cache;
    auto existingEntry { cache.find (argKey) };
    if (existingEntry != cache.end ())
        return existingEntry->second;
    return cache.emplace (argKey, CFStringCreateWithCString (kCFAllocatorDefault, std::to_string (argKey).c_str (), kCFStringEncodingUTF8)).first->second;
}



ARAIPCCFMessageEncoder::ARAIPCCFMessageEncoder ()
: _dictionary { CFDictionaryCreateMutable (kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) }
{}

ARAIPCCFMessageEncoder::~ARAIPCCFMessageEncoder ()
{
    CFRelease (_dictionary);
}

void ARAIPCCFMessageEncoder::appendInt32 (ARAIPCMessageKey argKey, int32_t argValue)
{
    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberSInt32Type, &argValue) };
    CFDictionarySetValue (_dictionary, _getEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void ARAIPCCFMessageEncoder::appendInt64 (ARAIPCMessageKey argKey, int64_t argValue)
{
    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberSInt64Type, &argValue) };
    CFDictionarySetValue (_dictionary, _getEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void ARAIPCCFMessageEncoder::appendSize (ARAIPCMessageKey argKey, size_t argValue)
{
    static_assert (sizeof (SInt64) == sizeof (size_t), "integer type needs adjustment for this compiler setup");

    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberSInt64Type, &argValue) };
    CFDictionarySetValue (_dictionary, _getEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void ARAIPCCFMessageEncoder::appendFloat (ARAIPCMessageKey argKey, float argValue)
{
    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberFloatType, &argValue) };
    CFDictionarySetValue (_dictionary, _getEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void ARAIPCCFMessageEncoder::appendDouble (ARAIPCMessageKey argKey, double argValue)
{
    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberDoubleType, &argValue) };
    CFDictionarySetValue (_dictionary, _getEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void ARAIPCCFMessageEncoder::appendString (ARAIPCMessageKey argKey, const char * argValue)
{
    auto argObject { CFStringCreateWithCString (kCFAllocatorDefault, argValue, kCFStringEncodingUTF8) };
    CFDictionarySetValue (_dictionary, _getEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void ARAIPCCFMessageEncoder::appendBytes (ARAIPCMessageKey argKey, const uint8_t * argValue, size_t argSize, bool copy)
{
    CFDataRef argObject;
    if (copy)
        argObject = CFDataCreate (kCFAllocatorDefault, argValue, (CFIndex) argSize);
    else
        argObject = CFDataCreateWithBytesNoCopy (kCFAllocatorDefault, argValue, (CFIndex) argSize, kCFAllocatorNull);
    CFDictionarySetValue (_dictionary, _getEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

ARAIPCMessageEncoder* ARAIPCCFMessageEncoder::appendSubMessage (ARAIPCMessageKey argKey)
{
    auto argObject = new ARAIPCCFMessageEncoder {};
    CFDictionarySetValue (_dictionary, _getEncodedKey (argKey), argObject->_dictionary);
    return argObject;
}

__attribute__((cf_returns_retained)) CFMutableDictionaryRef ARAIPCCFMessageEncoder::copyDictionary () const
{
    CFRetain (_dictionary);
    return _dictionary;
}

__attribute__((cf_returns_retained)) CFDataRef ARAIPCCFMessageEncoder::createMessageEncoderData ()  const
{
    if (CFDictionaryGetCount (_dictionary) == 0)
        return nullptr;
    return CFPropertyListCreateData (kCFAllocatorDefault, _dictionary, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
}


ARAIPCCFMessageDecoder::ARAIPCCFMessageDecoder (CFDictionaryRef dictionary)
: ARAIPCCFMessageDecoder::ARAIPCCFMessageDecoder (dictionary, true)
{}

ARAIPCCFMessageDecoder::ARAIPCCFMessageDecoder (CFDictionaryRef dictionary, bool retain)
: _dictionary { dictionary }
{
    if (dictionary && retain)
        CFRetain (dictionary);
}

ARAIPCCFMessageDecoder::~ARAIPCCFMessageDecoder ()
{
    if (_dictionary)
        CFRelease (_dictionary);
}

bool ARAIPCCFMessageDecoder::readInt32 (ARAIPCMessageKey argKey, int32_t* argValue) const
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

bool ARAIPCCFMessageDecoder::readInt64 (ARAIPCMessageKey argKey, int64_t* argValue) const
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

bool ARAIPCCFMessageDecoder::readSize (ARAIPCMessageKey argKey, size_t* argValue) const
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

bool ARAIPCCFMessageDecoder::readFloat (ARAIPCMessageKey argKey, float* argValue) const
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

bool ARAIPCCFMessageDecoder::readDouble (ARAIPCMessageKey argKey, double* argValue) const
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

bool ARAIPCCFMessageDecoder::readString (ARAIPCMessageKey argKey, const char ** argValue) const
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

bool ARAIPCCFMessageDecoder::readBytesSize (ARAIPCMessageKey argKey, size_t* argSize) const
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

void ARAIPCCFMessageDecoder::readBytes (ARAIPCMessageKey argKey, uint8_t* argValue) const
{
    ARA_INTERNAL_ASSERT (_dictionary);
    auto bytes { (CFDataRef) CFDictionaryGetValue (_dictionary, _getEncodedKey (argKey)) };
    ARA_INTERNAL_ASSERT (bytes && (CFGetTypeID (bytes) == CFDataGetTypeID ()));
    const auto length { CFDataGetLength (bytes) };
    CFDataGetBytes (bytes, CFRangeMake (0, length), argValue);
}

ARAIPCMessageDecoder* ARAIPCCFMessageDecoder::readSubMessage (ARAIPCMessageKey argKey) const
{
    auto dictionary { (CFDictionaryRef) CFDictionaryGetValue (_dictionary, _getEncodedKey (argKey)) };
    ARA_INTERNAL_ASSERT (!dictionary || (CFGetTypeID (dictionary) == CFDictionaryGetTypeID ()));
    if (dictionary == nullptr)
        return nullptr;
    return new ARAIPCCFMessageDecoder { dictionary };
}

bool ARAIPCCFMessageDecoder::hasDataForKey (ARAIPCMessageKey argKey) const
{
    return CFDictionaryContainsKey (_dictionary, _getEncodedKey (argKey));
}

ARAIPCCFMessageDecoder* ARAIPCCFMessageDecoder::createWithMessageData (CFDataRef messageData)
{
    if (CFDataGetLength (messageData) == 0)
        return nullptr;

    auto dictionary { (CFDictionaryRef) CFPropertyListCreateWithData (kCFAllocatorDefault, messageData, kCFPropertyListImmutable, nullptr, nullptr) };
    ARA_INTERNAL_ASSERT (dictionary && (CFGetTypeID (dictionary) == CFDictionaryGetTypeID ()));
    return new ARAIPCCFMessageDecoder { dictionary, false };
}

#if defined (__APPLE__)
    _Pragma ("GCC diagnostic pop")
#endif

}   // namespace IPC
}   // namespace ARA

#endif // ARA_ENABLE_IPC && defined (__APPLE__)
