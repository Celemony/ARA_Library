//------------------------------------------------------------------------------
//! \file       ARAIPCCFEncoding.cpp
//!             Implementation of ARAIPCMessageEn-/Decoder backed by CF(Mutable)Dictionary
//! \project    ARA SDK Examples
//! \copyright  Copyright (c) 2021-2022, Celemony Software GmbH, All Rights Reserved.
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
extern "C" {


#if defined (__GNUC__)
    _Pragma ("GCC diagnostic push")
    _Pragma ("GCC diagnostic ignored \"-Wold-style-cast\"")
#endif


// the message ID will be added to the underlying dictionary with a key that does not conflict with other message keys
constexpr ARAIPCMessageKey messageIDKey { -1 };


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
CFStringRef ARA_CALL ARAIPCCFMessageGetEncodedKey (ARAIPCMessageKey argKey, bool isInternalCall = false)
{
    if (!isInternalCall)
        ARA_INTERNAL_ASSERT (argKey >= 0);

    // \todo All plist formats available for CFPropertyListCreateData () in createEncodedMessage () need CFString keys.
    //       Once we switch to the more modern (NS)XPC API we shall be able to use CFNumber keys directly...
    static std::map<ARAIPCMessageKey, _CFReleaser> cache;
    auto existingEntry { cache.find (argKey) };
    if (existingEntry != cache.end ())
        return existingEntry->second;
    return cache.emplace (argKey, CFStringCreateWithCString (kCFAllocatorDefault, std::to_string (argKey).c_str (), kCFStringEncodingUTF8)).first->second;
}



inline ARAIPCMessageEncoderRef ARAIPCCFMessageToEncoderRef (CFMutableDictionaryRef dictionary)
{
    return (ARAIPCMessageEncoderRef) dictionary;
}

inline CFMutableDictionaryRef ARAIPCCFMessageFromEncoderRef (ARAIPCMessageEncoderRef messageEncoderRef)
{
    return (CFMutableDictionaryRef) messageEncoderRef;
}

void ARA_CALL ARAIPCCFMessageDestroyEncoder (ARAIPCMessageEncoderRef messageEncoderRef)
{
    if (messageEncoderRef)
        CFRelease (ARAIPCCFMessageFromEncoderRef (messageEncoderRef));
}

void ARA_CALL ARAIPCCFMessageAppendInt32 (ARAIPCMessageEncoderRef messageEncoderRef, ARAIPCMessageKey argKey, int32_t argValue)
{
    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberSInt32Type, &argValue) };
    CFDictionarySetValue (ARAIPCCFMessageFromEncoderRef (messageEncoderRef), ARAIPCCFMessageGetEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void ARA_CALL ARAIPCCFMessageAppendInt64 (ARAIPCMessageEncoderRef messageEncoderRef, ARAIPCMessageKey argKey, int64_t argValue)
{
    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberSInt64Type, &argValue) };
    CFDictionarySetValue (ARAIPCCFMessageFromEncoderRef (messageEncoderRef), ARAIPCCFMessageGetEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void ARA_CALL ARAIPCCFMessageAppendSize (ARAIPCMessageEncoderRef messageEncoderRef, ARAIPCMessageKey argKey, size_t argValue)
{
    static_assert (sizeof (SInt64) == sizeof (size_t), "integer type needs adjustment for this compiler setup");

    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberSInt64Type, &argValue) };
    CFDictionarySetValue (ARAIPCCFMessageFromEncoderRef (messageEncoderRef), ARAIPCCFMessageGetEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void ARA_CALL ARAIPCCFMessageAppendFloat (ARAIPCMessageEncoderRef messageEncoderRef, ARAIPCMessageKey argKey, float argValue)
{
    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberFloatType, &argValue) };
    CFDictionarySetValue (ARAIPCCFMessageFromEncoderRef (messageEncoderRef), ARAIPCCFMessageGetEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void ARA_CALL ARAIPCCFMessageAppendDouble (ARAIPCMessageEncoderRef messageEncoderRef, ARAIPCMessageKey argKey, double argValue)
{
    auto argObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberDoubleType, &argValue) };
    CFDictionarySetValue (ARAIPCCFMessageFromEncoderRef (messageEncoderRef), ARAIPCCFMessageGetEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void ARA_CALL ARAIPCCFMessageAppendString (ARAIPCMessageEncoderRef messageEncoderRef, ARAIPCMessageKey argKey, const char* argValue)
{
    auto argObject { CFStringCreateWithCString (kCFAllocatorDefault, argValue, kCFStringEncodingUTF8) };
    CFDictionarySetValue (ARAIPCCFMessageFromEncoderRef (messageEncoderRef), ARAIPCCFMessageGetEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

void ARA_CALL ARAIPCCFMessageAppendBytes (ARAIPCMessageEncoderRef messageEncoderRef, ARAIPCMessageKey argKey, const uint8_t* argValue, size_t argSize, bool copy)
{
    CFDataRef argObject;
    if (copy)
        argObject = CFDataCreate (kCFAllocatorDefault, argValue, (CFIndex) argSize);
    else
        argObject = CFDataCreateWithBytesNoCopy (kCFAllocatorDefault, argValue, (CFIndex) argSize, kCFAllocatorNull);
    CFDictionarySetValue (ARAIPCCFMessageFromEncoderRef (messageEncoderRef), ARAIPCCFMessageGetEncodedKey (argKey), argObject);
    CFRelease (argObject);
}

ARAIPCMessageEncoderRef ARA_CALL ARAIPCCFMessageAppendSubMessage (ARAIPCMessageEncoderRef messageEncoderRef, ARAIPCMessageKey argKey)
{
    auto argObject { CFDictionaryCreateMutable (kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
    CFDictionarySetValue (ARAIPCCFMessageFromEncoderRef (messageEncoderRef), ARAIPCCFMessageGetEncodedKey (argKey), argObject);
    return (ARAIPCMessageEncoderRef) argObject;
}

ARAIPCMessageEncoder ARAIPCCFCreateMessageEncoder (void)
{
    static const ARA::IPC::ARAIPCMessageEncoderInterface encoderMethods
    {
        ARAIPCCFMessageDestroyEncoder,
        ARAIPCCFMessageAppendInt32,
        ARAIPCCFMessageAppendInt64,
        ARAIPCCFMessageAppendSize,
        ARAIPCCFMessageAppendFloat,
        ARAIPCCFMessageAppendDouble,
        ARAIPCCFMessageAppendString,
        ARAIPCCFMessageAppendBytes,
        ARAIPCCFMessageAppendSubMessage
    };

    return { ARAIPCCFMessageToEncoderRef (CFDictionaryCreateMutable (kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)), &encoderMethods };
}

__attribute__((cf_returns_retained)) CFDictionaryRef ARAIPCCFCopyMessageEncoderDictionary (ARAIPCMessageEncoderRef messageEncoderRef)
{
    const auto dictionary { ARAIPCCFMessageFromEncoderRef (messageEncoderRef) };
    ARA_INTERNAL_ASSERT (dictionary);
    CFRetain (dictionary);
    return dictionary;
}

__attribute__((cf_returns_retained)) CFDictionaryRef ARAIPCCFCopyMessageEncoderDictionaryAddingMessageID (ARAIPCMessageEncoderRef messageEncoderRef, ARAIPCMessageID messageIDValue)
{
    static_assert (std::is_same<ARAIPCMessageID, SInt32>::value, "encoding needs to be adopted here if key type is changed");
    auto messageIDObject { CFNumberCreate (kCFAllocatorDefault, kCFNumberSInt32Type, &messageIDValue) };
    auto dictionary { ARAIPCCFMessageFromEncoderRef (messageEncoderRef) };
    ARA_INTERNAL_ASSERT (dictionary);
    CFDictionarySetValue (dictionary, ARAIPCCFMessageGetEncodedKey (messageIDKey, true), messageIDObject);
    CFRelease (messageIDObject);
    CFRetain (dictionary);
    return dictionary;
}

__attribute__((cf_returns_retained)) CFDataRef ARAIPCCFCreateMessageEncoderData (ARAIPCMessageEncoderRef messageEncoderRef)
{
    const auto dictionary { ARAIPCCFMessageFromEncoderRef (messageEncoderRef) };
    if ((!dictionary) || (CFDictionaryGetCount (dictionary) == 0))
        return nullptr;
    return CFPropertyListCreateData (kCFAllocatorDefault, dictionary, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
}



inline ARAIPCMessageDecoderRef ARAIPCCFMessageToDecoderRef (CFDictionaryRef dictionary)
{
    return (ARAIPCMessageDecoderRef) dictionary;
}

inline CFDictionaryRef ARAIPCCFMessageFromDecoderRef (ARAIPCMessageDecoderRef messageDecoderRef)
{
    return (CFDictionaryRef) messageDecoderRef;
}

void ARA_CALL ARAIPCCFMessageDestroyDecoder (ARAIPCMessageDecoderRef messageDecoderRef)
{
    if (messageDecoderRef)
        CFRelease (ARAIPCCFMessageFromDecoderRef (messageDecoderRef));
}

bool ARA_CALL ARAIPCCFMessageIsEmpty (ARAIPCMessageDecoderRef messageDecoderRef)
{
    return (!messageDecoderRef) || (CFDictionaryGetCount (ARAIPCCFMessageFromDecoderRef (messageDecoderRef)) == 0);
}

bool ARA_CALL ARAIPCCFMessageReadInt32 (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey, int32_t* argValue)
{
    CFNumberRef number {};
    if (messageDecoderRef)
        number = (CFNumberRef) CFDictionaryGetValue (ARAIPCCFMessageFromDecoderRef (messageDecoderRef), ARAIPCCFMessageGetEncodedKey (argKey));
    if (!number)
    {
        *argValue = 0;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (number) == CFNumberGetTypeID ());
    CFNumberGetValue (number, kCFNumberSInt32Type, argValue);
    return true;
}

bool ARA_CALL ARAIPCCFMessageReadInt64 (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey, int64_t* argValue)
{
    CFNumberRef number {};
    if (messageDecoderRef)
        number = (CFNumberRef) CFDictionaryGetValue (ARAIPCCFMessageFromDecoderRef (messageDecoderRef), ARAIPCCFMessageGetEncodedKey (argKey));
    if (!number)
    {
        *argValue = 0;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (number) == CFNumberGetTypeID ());
    CFNumberGetValue (number, kCFNumberSInt64Type, argValue);
    return true;
}

bool ARA_CALL ARAIPCCFMessageReadSize (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey, size_t* argValue)
{
    static_assert (sizeof (SInt64) == sizeof (size_t), "integer type needs adjustment for this compiler setup");

    CFNumberRef number {};
    if (messageDecoderRef)
        number = (CFNumberRef) CFDictionaryGetValue (ARAIPCCFMessageFromDecoderRef (messageDecoderRef), ARAIPCCFMessageGetEncodedKey (argKey));
    if (!number)
    {
        *argValue = 0;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (number) == CFNumberGetTypeID ());
    CFNumberGetValue (number, kCFNumberSInt64Type, argValue);
    return true;
}

bool ARA_CALL ARAIPCCFMessageReadFloat (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey, float* argValue)
{
    CFNumberRef number {};
    if (messageDecoderRef)
        number = (CFNumberRef) CFDictionaryGetValue (ARAIPCCFMessageFromDecoderRef (messageDecoderRef), ARAIPCCFMessageGetEncodedKey (argKey));
    if (!number)
    {
        *argValue = 0.0f;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (number) == CFNumberGetTypeID ());
    CFNumberGetValue (number, kCFNumberFloatType, argValue);
    return true;
}

bool ARA_CALL ARAIPCCFMessageReadDouble (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey, double* argValue)
{
    CFNumberRef number {};
    if (messageDecoderRef)
        number = (CFNumberRef) CFDictionaryGetValue (ARAIPCCFMessageFromDecoderRef (messageDecoderRef), ARAIPCCFMessageGetEncodedKey (argKey));
    if (!number)
    {
        *argValue = 0.0;
        return false;
    }
    ARA_INTERNAL_ASSERT (CFGetTypeID (number) == CFNumberGetTypeID ());
    CFNumberGetValue (number, kCFNumberDoubleType, argValue);
    return true;
}

bool ARA_CALL ARAIPCCFMessageReadString (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey, const char** argValue)
{
    CFStringRef string {};
    if (messageDecoderRef)
        string = (CFStringRef) CFDictionaryGetValue (ARAIPCCFMessageFromDecoderRef (messageDecoderRef), ARAIPCCFMessageGetEncodedKey (argKey));
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

bool ARA_CALL ARAIPCCFMessageReadBytesSize (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey, size_t* argSize)
{
    CFDataRef bytes {};
    if (messageDecoderRef)
        bytes = (CFDataRef) CFDictionaryGetValue (ARAIPCCFMessageFromDecoderRef (messageDecoderRef), ARAIPCCFMessageGetEncodedKey (argKey));
    if (!bytes)
    {
        *argSize = 0;
        return false;
    }
    ARA_INTERNAL_ASSERT (bytes && (CFGetTypeID (bytes) == CFDataGetTypeID ()));
    *argSize = (size_t) CFDataGetLength (bytes);
    return true;

}

void ARA_CALL ARAIPCCFMessageReadBytes (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey, uint8_t* argValue)
{
    ARA_INTERNAL_ASSERT (messageDecoderRef);
    auto bytes { (CFDataRef) CFDictionaryGetValue (ARAIPCCFMessageFromDecoderRef (messageDecoderRef), ARAIPCCFMessageGetEncodedKey (argKey)) };
    ARA_INTERNAL_ASSERT (bytes && (CFGetTypeID (bytes) == CFDataGetTypeID ()));
    const auto length { CFDataGetLength (bytes) };
    CFDataGetBytes (bytes, CFRangeMake (0, length), argValue);
}

ARAIPCMessageDecoderRef ARA_CALL ARAIPCCFMessageReadSubMessage (ARAIPCMessageDecoderRef messageDecoderRef, ARAIPCMessageKey argKey)
{
    auto dictionary { (CFDictionaryRef) CFDictionaryGetValue (ARAIPCCFMessageFromDecoderRef (messageDecoderRef), ARAIPCCFMessageGetEncodedKey (argKey)) };
    ARA_INTERNAL_ASSERT (!dictionary || (CFGetTypeID (dictionary) == CFDictionaryGetTypeID ()));
    if (dictionary)
        CFRetain (dictionary);
    return ARAIPCCFMessageToDecoderRef (dictionary);
}

ARAIPCMessageDecoder ARAIPCCFCreateMessageDecoderWithRetainedDictionary (CFDictionaryRef __attribute__((cf_consumed)) messageDictionary)
{
    static const ARA::IPC::ARAIPCMessageDecoderInterface decoderMethods
    {
        ARAIPCCFMessageDestroyDecoder,
        ARAIPCCFMessageIsEmpty,
        ARAIPCCFMessageReadInt32,
        ARAIPCCFMessageReadInt64,
        ARAIPCCFMessageReadSize,
        ARAIPCCFMessageReadFloat,
        ARAIPCCFMessageReadDouble,
        ARAIPCCFMessageReadString,
        ARAIPCCFMessageReadBytesSize,
        ARAIPCCFMessageReadBytes,
        ARAIPCCFMessageReadSubMessage
    };

    return { ARAIPCCFMessageToDecoderRef (messageDictionary), &decoderMethods };
}

ARAIPCMessageDecoder ARAIPCCFCreateMessageDecoderWithDictionary (CFDictionaryRef messageDictionary)
{
    if (messageDictionary)
        CFRetain (messageDictionary);

    return ARAIPCCFCreateMessageDecoderWithRetainedDictionary (messageDictionary);
}

ARAIPCMessageID ARAIPCCFGetMessageIDFromDictionary (ARAIPCMessageDecoderRef messageDecoderRef)
{
    const auto dictionary { ARAIPCCFMessageFromDecoderRef (messageDecoderRef) };
    ARA_INTERNAL_ASSERT (dictionary != nullptr);
    const auto number { (CFNumberRef) CFDictionaryGetValue (dictionary, ARAIPCCFMessageGetEncodedKey (messageIDKey, true)) };
    ARA_INTERNAL_ASSERT (number != nullptr);
    ARA_INTERNAL_ASSERT (CFGetTypeID (number) == CFNumberGetTypeID ());
    static_assert (std::is_same<ARAIPCMessageID, SInt32>::value, "decoding needs to be adopted here if key type is changed");
    ARAIPCMessageID result;
    CFNumberGetValue (number, kCFNumberSInt32Type, &result);
    return result;
}

ARAIPCMessageDecoder ARAIPCCFCreateMessageDecoder (CFDataRef messageData)
{
    if (CFDataGetLength (messageData) == 0)
        return ARAIPCCFCreateMessageDecoderWithRetainedDictionary (nullptr);

    auto dictionary { (CFDictionaryRef) CFPropertyListCreateWithData (kCFAllocatorDefault, messageData, kCFPropertyListImmutable, nullptr, nullptr) };
    ARA_INTERNAL_ASSERT (dictionary && (CFGetTypeID (dictionary) == CFDictionaryGetTypeID ()));
    auto result { ARAIPCCFCreateMessageDecoderWithRetainedDictionary (dictionary) };
    return result;
}


#if defined (__APPLE__)
    _Pragma ("GCC diagnostic pop")
#endif

}   // extern "C"
}   // namespace IPC
}   // namespace ARA

#endif // ARA_ENABLE_IPC && defined (__APPLE__)
