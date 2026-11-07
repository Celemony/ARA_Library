//------------------------------------------------------------------------------
//! \file       ARAIPCEncoding.h
//!             Implementation helpers shared by both the ARA IPC proxy host and plug-in
//!             Typically, this file is not included directly - either ARAIPCProxyHost.h
//!             ARAIPCProxyPlugIn.h will be used instead.
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

#ifndef ARAIPCEncoding_h
#define ARAIPCEncoding_h

#include "ARA_Library/IPC/ARAIPCConnection.h"

#if ARA_ENABLE_IPC


#include "ARA_Library/Debug/ARADebug.h"
#include "ARA_Library/Dispatch/ARAContentReader.h"
#include "ARA_Library/Utilities/ARAChannelFormat.h"

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <optional>


//! @addtogroup ARA_Library_IPC
//! @{

namespace ARA {
namespace IPC {


//------------------------------------------------------------------------------
// wrapper factories to efficiently handle sending and receiving raw bytes
//------------------------------------------------------------------------------

// providing an array of bytes for sending:
// byte count, pointer to bytes, and flag whether to copy or reference the bytes
// copy should be set to false whenever the bytes are guaranteed to remain valid
// until the message has been sent - this is the case for all blocking sends,
// but not for non-blocking sends, and depends on context for replies

struct BytesEncoder
{
    const uint8_t* bytes;
    size_t size;
    bool copy;

    BytesEncoder (const uint8_t* const data, const size_t dataSize, const bool copyData)
    : bytes { data },
      size { dataSize },
      copy { copyData }
    {}

    BytesEncoder (const std::vector<uint8_t>& data, const bool copyData)
    : BytesEncoder { data.data (), data.size (), copyData }
    {}
};

// evaluating an received array of bytes
// the callback must decide whether to copy the bytes or merely reference them,
// depending on whether its evaluation will be completed before the message is destroyed -
// e.g call arguments are typically only used during the call
// the callback can return false if the data size does not match the current contest,
// which will indicate failure and abort the decoding
class BytesDecoder : public std::function<bool (const uint8_t* data, size_t dataSize)>
{
public:
    // obtain reference to message data
    BytesDecoder (const uint8_t*& bytes, size_t& size)
    : std::function<bool (const uint8_t* data, size_t dataSize)> {
        [&bytes, &size] (const uint8_t* data, size_t dataSize) -> bool
        {
            bytes = data;
            size = dataSize;
            return true;
        } }
    {}

    // copy to a provided buffer, with size being updated
    BytesDecoder (uint8_t* bytes, size_t& size)
    : std::function<bool (const uint8_t* data, size_t dataSize)> {
        [bytes, &size] (const uint8_t* data, size_t dataSize) -> bool
        {
            if (dataSize > size)    // if there is more data then we can take, abort
                return false;

            std::memcpy (bytes, data, dataSize);
            size = dataSize;
            return true;
        } }
    {}

    // copy to a provided std::vector of bytes
    BytesDecoder (std::vector<uint8_t>& bytes)
    : std::function<bool (const uint8_t* data, size_t dataSize)> {
        [&bytes] (const uint8_t* data, size_t dataSize) -> bool
        {
            bytes.resize (dataSize);
            std::memcpy (bytes.data (), data, dataSize);
            return true;
        } }
    {}
};


//------------------------------------------------------------------------------
// wrapper factories to efficiently handle sending and receiving arrays
//------------------------------------------------------------------------------


template<typename ElementT>
struct ArrayArgument
{
    static_assert (sizeof (ElementT) > sizeof (ARAByte), "byte-sized arrays should be sent as raw bytes");
    ElementT* elements;
    size_t count;
};


//------------------------------------------------------------------------------
// various private helpers
//------------------------------------------------------------------------------

// private helper template to detect ARA ref types
template<typename T>
inline constexpr bool _IsRefType { false };
#define ARA_IPC_SPECIALIZE_FOR_REF_TYPE(Type)   \
template<>                                      \
inline constexpr bool _IsRefType<Type> { true };
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAMusicalContextRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARARegionSequenceRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAAudioSourceRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAAudioModificationRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAPlaybackRegionRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAContentReaderRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARADocumentControllerRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAPlaybackRendererRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAEditorRendererRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAEditorViewRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAPlugInExtensionRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAMusicalContextHostRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARARegionSequenceHostRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAAudioSourceHostRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAAudioModificationHostRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAPlaybackRegionHostRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAContentReaderHostRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAAudioAccessControllerHostRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAAudioReaderHostRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAArchivingControllerHostRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAArchiveReaderHostRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAArchiveWriterHostRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAContentAccessControllerHostRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAModelUpdateControllerHostRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAPlaybackControllerHostRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAIPCProxyHostRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAIPCProxyPlugInRef)
ARA_IPC_SPECIALIZE_FOR_REF_TYPE (ARAIPCPlugInInstanceRef)
#undef ARA_IPC_SPECIALIZE_FOR_REF_TYPE


//------------------------------------------------------------------------------
// private primitive wrappers for writing to outgoing and reading from incoming messages
//------------------------------------------------------------------------------


// primitives for appending an argument to a message
template <typename T>
inline void _appendNumberToMessage (OutgoingMessage* message, T argValue)
{
    // \todo potential endianness conversion is missing here if needed...
    message->appendBytes (reinterpret_cast<const uint8_t *> (&argValue), sizeof (argValue), true);
}
inline void _appendToMessage (OutgoingMessage* message, const int32_t argValue)
{
    _appendNumberToMessage (message, argValue);
}
inline void _appendToMessage (OutgoingMessage* message, const int64_t argValue)
{
    _appendNumberToMessage (message, argValue);
}
inline void _appendToMessage (OutgoingMessage* message, const size_t argValue)
{
    _appendNumberToMessage (message, argValue);
}
inline void _appendToMessage (OutgoingMessage* message, const float argValue)
{
    _appendNumberToMessage (message, argValue);
}
inline void _appendToMessage (OutgoingMessage* message, const double argValue)
{
    _appendNumberToMessage (message, argValue);
}
inline void _appendToMessage (OutgoingMessage* message, const char* const argValue)
{
    if (argValue)
    {
        const auto size { std::strlen (argValue) + 1 };     // includes terminator
        _appendNumberToMessage (message, size);
        message->appendBytes (reinterpret_cast<const uint8_t *> (argValue), size, true);
    }
    else
    {
        _appendToMessage (message, static_cast<size_t> (0));
    }
}
inline void _appendToMessage (OutgoingMessage* message, const BytesEncoder& argValue)
{
    ARA_INTERNAL_ASSERT ((argValue.bytes != nullptr) || (argValue.size == 0));
    _appendNumberToMessage (message, argValue.size);
    if (argValue.size)
        message->appendBytes (argValue.bytes, argValue.size, argValue.copy);
}

// primitives for reading an argument from a message
template <typename T>
inline bool _readNumberFromMessage (const IncomingMessage* message, T& argValue)
{
    constexpr auto size { sizeof (T) };
    if (const auto bytes { message->readBytes (size) })
    {
        argValue = *reinterpret_cast<const T*> (bytes);
        return true;
    }
    else
    {
        argValue = T { 0 };
        return false;
    }
}
inline bool _readFromMessage (const IncomingMessage* message, int32_t& argValue)
{
    return _readNumberFromMessage (message, argValue);
}
inline bool _readFromMessage (const IncomingMessage* message, int64_t& argValue)
{
    return _readNumberFromMessage (message, argValue);
}
inline bool _readFromMessage (const IncomingMessage* message, size_t& argValue)
{
    return _readNumberFromMessage (message, argValue);
}
inline bool _readFromMessage (const IncomingMessage* message, float& argValue)
{
    return _readNumberFromMessage (message, argValue);
}
inline bool _readFromMessage (const IncomingMessage* message, double& argValue)
{
    return _readNumberFromMessage (message, argValue);
}
inline bool _readFromMessage (const IncomingMessage* message, BytesDecoder& argValue)
{
    size_t size;
    if (!_readFromMessage (message, size))
        return false;

    if (size == 0)
        return argValue (nullptr, 0);

    if (const auto bytes { message->readBytes (size) })
        return argValue (bytes, size);
    else
        return false;
}
inline bool _readFromMessage (const IncomingMessage* message, const char*& argValue)
{
    size_t size;
    // \todo is there a safe/proper way across all compilers for this cast to avoid the copy?
//  BytesDecoder bytesDecoder { reinterpret_cast<const uint8_t*&> (argValue), size };
    const uint8_t* bytes;
    BytesDecoder bytesDecoder { bytes, size };
    if (_readFromMessage (message, bytesDecoder))
    {
        argValue = reinterpret_cast<const char*> (bytes);
        return true;
    }
    else
    {
        argValue = nullptr;
        return false;
    }
}


//------------------------------------------------------------------------------
// overloads of the IPCMessageEn-/Decoder primitives for types that can be
// directly mapped to a primitive type
//------------------------------------------------------------------------------


// templated overloads of the IPCMessageEn-/Decoder primitives for ARA (host) ref types,
// which are stored as size_t
template<typename T, std::enable_if_t<_IsRefType<T>, bool> = true>
inline void _appendToMessage (OutgoingMessage* message, const T argValue)
{
    _appendToMessage (message, reinterpret_cast<size_t> (argValue));
}
template<typename T, std::enable_if_t<_IsRefType<T>, bool> = true>
inline bool _readFromMessage (const IncomingMessage* message, T& argValue)
{
    // \todo is there a safe/proper way across all compilers for this cast to avoid the copy?
//  return message.readSize (argKey, *reinterpret_cast<size_t*> (&argValue));
    size_t tmp;
    const auto success { _readFromMessage (message, tmp) };
    argValue = reinterpret_cast<T> (tmp);
    return success;
}


//------------------------------------------------------------------------------
// private helper templates to en-/decode ARA API values
// The mapping is 1:1 except for ARA (host)refs which are encoded as size_t, and aggregate types
// (i.e. ARA structs or std::vector<> of types other than ARAByte), which are expressed as sub-messages.
// En- and Decoding use the same implementation technique:
// To support using compound types (arrays, structs) both as indexed sub-message for call arguments
// when sending as well as as root message for replies, there's a templated _ValueEn-/Decoder struct
// for each type which provides an encode&append/read&decode call that extracts a sub-message if
// needed and then performs the en/decode via a separate plain en-/decoding call only available in
// compound types. The latter call will be used directly for compound data type replies.
// In order not to have to spell out _ValueEn-/Decoder<> explicitly, overloaded wrapper function
// templates _encodeAndAppend() and _readAndDecode() are provided.
//------------------------------------------------------------------------------


// declarations of wrapper functions to implicitly deduce _ValueEn-/Decoder<> -
// they are defined further down below, after all specializations are defined
template <typename ValueT>
inline void _encodeAndAppend (OutgoingMessage* message, const ValueT& argValue);
template <typename ValueT>
inline bool _readAndDecode (ValueT& result, const IncomingMessage* message);


// primary templates for basic types (numbers, strings, (host)refs and raw bytes)
template<typename ValueT>
struct _ValueEncoder
{
    static inline void encodeAndAppend (OutgoingMessage* message, const ValueT& argValue)
    {
        _appendToMessage (message, argValue);
    }
};
template<typename ValueT>
struct _ValueDecoder
{
    static inline bool readAndDecode (ValueT& result, const IncomingMessage* message)
    {
        return _readFromMessage (message, result);
    }
};


// common base classes for en-/decoding compound types (arrays, structs) via nested messages,
// providing the generic encode&append/read&decode calls
template<typename ValueT>
struct _CompoundValueEncoderBase
{
    static inline void encodeAndAppend (OutgoingMessage* message, const ValueT& argValue)
    {
        _ValueEncoder<ValueT>::encode (message, argValue);
    }
};
template<typename ValueT>
struct _CompoundValueDecoderBase
{
    static inline bool readAndDecode (ValueT& result, const IncomingMessage* message)
    {
        return _ValueDecoder<ValueT>::decode (result, message);
    }
};


// specialization for encoding arrays (variable or fixed size)
template<typename ElementT>
struct _ValueEncoder<ArrayArgument<ElementT>> : public _CompoundValueEncoderBase<ArrayArgument<ElementT>>
{
    static inline void encode (OutgoingMessage* message, const ArrayArgument<ElementT>& value)
    {
        ARA_INTERNAL_ASSERT ((value.count == 0) || (value.elements != nullptr));
        _encodeAndAppend (message, value.count);
        for (size_t i { 0 }; i < value.count; ++i)
            _encodeAndAppend (message, value.elements[i]);
    }
};

// specialization for decoding fixed-size arrays
template<typename ElementT>
struct _ValueDecoder<ArrayArgument<ElementT>> : public _CompoundValueDecoderBase<ArrayArgument<ElementT>>
{
    static inline bool decode (ArrayArgument<ElementT>& result, const IncomingMessage* message)
    {
        bool success { true };
        size_t count;
        success &= _readAndDecode (count, message);
        success &= (count == result.count);
        if (count > result.count)
            count = result.count;

        for (size_t i { 0 }; i < count; ++i)
            success &= _readAndDecode (result.elements[i], message);
        return success;
    }
};

// specialization for decoding variable arrays
template<typename ElementT>
struct _ValueDecoder<std::vector<ElementT>> : public _CompoundValueDecoderBase<std::vector<ElementT>>
{
    static inline bool decode (std::vector<ElementT>& result, const IncomingMessage* message)
    {
        bool success { true };
        size_t count;
        success &= _readAndDecode (count, message);
        result.resize (count);
        for (size_t i { 0 }; i < count; ++i)
            success &= _readAndDecode (result[i], message);
        return success;
    }
};

// specialization for encoding optional types
template<typename ValueT>
struct _ValueEncoder<std::optional<ValueT>>
{
    static inline void encodeAndAppend (OutgoingMessage* message, const std::optional<ValueT>& argValue)
    {
        _appendToMessage (message, static_cast<int32_t> ((argValue) ? 1 : 0));
        if (argValue)
            _ValueEncoder<std::remove_const_t<ValueT>>::encodeAndAppend (message, *argValue);
    }
};

// specialization for decoding optional types
template<typename ValueT>
struct _ValueDecoder<std::optional<ValueT>>
{
    static inline bool readAndDecode (std::optional<ValueT>& result, const IncomingMessage* message)
    {
        result = std::nullopt;
        int32_t hasValue;
        if (!_readAndDecode (hasValue, message))
            return false;

        if (hasValue != 0)
        {
            ValueT value;
            if (!_readAndDecode (value, message))
                return false;
            result = std::make_optional<ValueT> (std::move (value));
        }
        return true;
    }
};


// specializations for en/decoding each ARA struct

// primary templates, must be specialized for all ARA structs that contain 4-byte values (ARAInt32, float),
// 8-byte values (size_t, ARA refs, double) and strings respectively
// \todo this must be adjusted if pointer-size (size_t, refs) should ever change
// \todo this should eventually be implemented using C++26 reflection
template<typename StructT>
struct _4ByteValueMemberOffsets
{
    static constexpr std::array<size_t, 0> _get () { return {{}}; }
};
template<typename StructT>
struct _8ByteValueMemberOffsets
{
    static constexpr std::array<size_t, 0> _get () { return {{}}; }
};
template<typename StructT>
struct _StringMemberOffsets
{
    static constexpr std::array<size_t, 0> _get () { return {{}}; }
};
#define ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1(StructT, Template, member1)                             \
template<> struct Template<StructT>                                                                 \
{ static constexpr std::array<size_t, 1> _get () { return {{ offsetof (StructT, member1) }}; }      \
};
#define ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2(StructT, Template, member1, member2)                    \
template<> struct Template<StructT>                                                                 \
{ static constexpr std::array<size_t, 2> _get () { return {{ offsetof (StructT, member1), offsetof (StructT, member2) }}; } \
};
#define ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_3(StructT, Template, member1, member2, member3)           \
template<> struct Template<StructT>                                                                 \
{ static constexpr std::array<size_t, 3> _get () { return {{ offsetof (StructT, member1), offsetof (StructT, member2), offsetof (StructT, member3) }}; } \
};
#define ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_4(StructT, Template, member1, member2, member3, member4) \
template<> struct Template<StructT>                                                                 \
{ static constexpr std::array<size_t, 4> _get () { return {{ offsetof (StructT, member1), offsetof (StructT, member2), offsetof (StructT, member3), offsetof (StructT, member4) }}; } \
};
#define ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_5(StructT, Template, member1, member2, member3, member4, member5) \
template<> struct Template<StructT>                                                                 \
{ static constexpr std::array<size_t, 5> _get () { return {{ offsetof (StructT, member1), offsetof (StructT, member2), offsetof (StructT, member3), offsetof (StructT, member4), offsetof (StructT, member5) }}; } \
};
#define ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_6(StructT, Template, member1, member2, member3, member4, member5, member6) \
template<> struct Template<StructT>                                                                 \
{ static constexpr std::array<size_t, 6> _get () { return {{ offsetof (StructT, member1), offsetof (StructT, member2), offsetof (StructT, member3), offsetof (StructT, member4), offsetof (StructT, member5), offsetof (StructT, member6) }}; } \
};
#define ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_7(StructT, Template, member1, member2, member3, member4, member5, member6, member7) \
template<> struct Template<StructT>                                                                 \
{ static constexpr std::array<size_t, 7> _get () { return {{ offsetof (StructT, member1), offsetof (StructT, member2), offsetof (StructT, member3), offsetof (StructT, member4), offsetof (StructT, member5), offsetof (StructT, member6), offsetof (StructT, member7) }}; } \
};

#define ARA_IPC_HAS_ADDENDUM_MEMBER(var, member)                                                    \
        /* \todo ARA_IMPLEMENTS_FIELD decorates the type with the ARA:: namespace,     */           \
        /* this conflicts with decltype's result - this copied version drops the ARA:: */           \
        (var.structSize > offsetof (std::remove_reference_t<decltype (var)>, member))

#define ARA_IPC_BEGIN_ENCODE_STRUCT(StructT, sizeExpr)                                              \
template<> struct _ValueEncoder<StructT> : public _CompoundValueEncoderBase<StructT>                \
{                                                                                                   \
    static inline void encode (OutgoingMessage* message, const StructT& value)                      \
    {   const auto structPtr { reinterpret_cast<const uint8_t*> (&value) };                         \
        const auto structSize { sizeExpr };                                                         \
        [[maybe_unused]] const auto data { message->appendBytes (structPtr, structSize, true) };    \
        ARA_INTERNAL_ASSERT (data != nullptr);                                                      \
        [[maybe_unused]] std::vector<BytesEncoder> pendingBytes;
#define ARA_IPC_BEGIN_ENCODE_FIXED_STRUCT(StructT)                                                  \
        ARA_IPC_BEGIN_ENCODE_STRUCT (StructT, sizeof (StructT))
#define ARA_IPC_BEGIN_ENCODE_SIZED_STRUCT(StructT)                                                  \
        ARA_IPC_BEGIN_ENCODE_STRUCT (StructT, value.structSize)

#define ARA_IPC_ENCODE_VARIABLE_ARRAY(member, count)                                                \
        if (value.count > 0)                                                                        \
            pendingBytes.emplace_back (reinterpret_cast<const uint8_t*> (value.member), value.count * sizeof (*value.member), true);
#define ARA_IPC_ENCODE_ADDENDUM_VARIABLE_ARRAY(member, count)                                       \
        if (ARA_IPC_HAS_ADDENDUM_MEMBER (value, member))                                            \
        {   ARA_IPC_ENCODE_VARIABLE_ARRAY (member, count)                                           \
        }
#define ARA_IPC_ENCODE_VARIABLE_STRING_ARRAY(member, count)                                         \
        /* because this is only used on the main thread, a simple static works here */              \
        static std::vector<size_t> member##Sizes;                                                   \
        member##Sizes.clear ();                                                                     \
        member##Sizes.reserve (value.count);                                                        \
        for (size_t i { 0 }; i < value.count; ++i)                                                  \
        {   const auto stringSize { std::strlen (value.member[i]) + 1 };  /* includes terminator */ \
            member##Sizes.emplace_back (stringSize);   /* no reallocation due to reserve() above */ \
            pendingBytes.emplace_back (reinterpret_cast<const uint8_t*> (member##Sizes.data () + i), sizeof (size_t), true); \
            pendingBytes.emplace_back (reinterpret_cast<const uint8_t*> (value.member[i]), stringSize, true); \
        }
#define ARA_IPC_ENCODE_ADDENDUM_VARIABLE_STRING_ARRAY(member, count)                                \
        if (ARA_IPC_HAS_ADDENDUM_MEMBER (value, member))                                            \
        {   ARA_IPC_ENCODE_VARIABLE_STRING_ARRAY (member, count)                                    \
        }

// careful: these currently simply memcpy the struct, which only works for very simple ones without strings etc.
//          \todo restructure code to allow for properly recursively en-/decoding structs
#define ARA_IPC_ENCODE_STRUCT_PTR_MEMBER(member)                                                    \
        if (value.member != nullptr)                                                                \
            pendingBytes.emplace_back (reinterpret_cast<const uint8_t*> (value.member), sizeof (*value.member), true);
#define ARA_IPC_ENCODE_ADDENDUM_STRUCT_PTR_MEMBER(member)                                           \
        if (ARA_IPC_HAS_ADDENDUM_MEMBER (value, member))                                            \
        {   ARA_IPC_ENCODE_STRUCT_PTR_MEMBER (member)                                               \
        }

#define ARA_IPC_END_ENCODE_STRUCT(StructT)                                                          \
        for (auto offset : _StringMemberOffsets<StructT>::_get ())                                  \
            if (offset < structSize)                                                                \
                if (auto stringPtr { *reinterpret_cast<const char* const*> (structPtr + offset) })  \
                {   const auto stringSize { std::strlen (stringPtr) + 1 };  /* includes terminator */ \
                    pendingBytes.emplace_back (reinterpret_cast<const uint8_t*> (stringPtr), stringSize, true); \
                    *reinterpret_cast<size_t*> (data + offset) = stringSize;                        \
                }                                                                                   \
        /* we begin appending here, which invalidates our data ptr! */                              \
        for (auto pending : pendingBytes)                                                           \
            message->appendBytes (pending.bytes, pending.size, pending.copy);                       \
    }                                                                                               \
};
#define ARA_IPC_END_ENCODE_FIXED_STRUCT(StructT)                                                    \
        ARA_IPC_END_ENCODE_STRUCT(StructT)
#define ARA_IPC_END_ENCODE_SIZED_STRUCT(StructT)                                                    \
        ARA_IPC_END_ENCODE_STRUCT(StructT)


#define ARA_IPC_BEGIN_DECODE_STRUCT(StructT, sizeExpr)                                              \
template<> struct _ValueDecoder<StructT> : public _CompoundValueDecoderBase<StructT>                \
{                                                                                                   \
    static inline bool decode (StructT& result, const IncomingMessage* message)                     \
    {   const auto structPtr { reinterpret_cast<uint8_t*> (&result) };                              \
        const auto data { message->readBytes (sizeExpr) };                                          \
        ARA_INTERNAL_ASSERT (data != nullptr);                                                      \
        if (data == nullptr)                                                                        \
            return false;                                                                           \
        std::memcpy (structPtr, data, sizeExpr);
#define ARA_IPC_BEGIN_DECODE_FIXED_STRUCT(StructT)                                                  \
        ARA_IPC_BEGIN_DECODE_STRUCT (StructT, sizeof (StructT))
#define ARA_IPC_BEGIN_DECODE_SIZED_STRUCT(StructT)                                                  \
        ARA_IPC_BEGIN_DECODE_STRUCT (StructT, k##StructT##MinSize)                                  \
        ARA_INTERNAL_ASSERT (result.structSize >= k##StructT##MinSize);                             \
        /* We rely on the IPC code always supporting all data types used in the receiver -   */     \
        /* if it doesn't fully decode/initialize the struct, access to addendums will crash. */     \
        /* Thus any addition to the API structs must be reflected here immediately.          */     \
        if (result.structSize > k##StructT##MinSize)                                                \
        {   const auto data2 { message->readBytes (result.structSize - k##StructT##MinSize) };      \
            ARA_INTERNAL_ASSERT (data2 != nullptr);                                                 \
            if (data2 == nullptr)                                                                   \
                return false;                                                                       \
            std::memcpy (structPtr + k##StructT##MinSize, data2, result.structSize - k##StructT##MinSize); \
        }

#define ARA_IPC_DECODE_STRUCT_PTR_MEMBER(member)                                                    \
        if (result.member != nullptr)                                                               \
        {   const auto memberData { message->readBytes (sizeof (*result.member)) };                 \
            ARA_INTERNAL_ASSERT (memberData != nullptr);                                            \
            result.member = reinterpret_cast<decltype (result.member)> (memberData);                \
            if (memberData == nullptr)                                                              \
                return false;                                                                       \
        }
#define ARA_IPC_DECODE_ADDENDUM_STRUCT_PTR_MEMBER(member)                                           \
        if (ARA_IPC_HAS_ADDENDUM_MEMBER (result, member))                                           \
        {   ARA_IPC_DECODE_STRUCT_PTR_MEMBER (member)                                               \
        }

#define ARA_IPC_DECODE_VARIABLE_ARRAY(member, count)                                                \
        if (result.count > 0)                                                                       \
        {   const auto memberData { message->readBytes (result.count * sizeof (*result.member)) };  \
            ARA_INTERNAL_ASSERT (memberData != nullptr);                                            \
            result.member = reinterpret_cast<decltype (result.member)> (memberData);                \
            if (memberData == nullptr)                                                              \
                return false;                                                                       \
        }
#define ARA_IPC_DECODE_ADDENDUM_VARIABLE_ARRAY(member, count)                                       \
        if (ARA_IPC_HAS_ADDENDUM_MEMBER (result, member))                                           \
        {   ARA_IPC_DECODE_VARIABLE_ARRAY (member, count)                                           \
        }
#define ARA_IPC_DECODE_VARIABLE_STRING_ARRAY(member, count)                                         \
        /* because this is only used on the main thread, a simple static works here */              \
        static std::vector<const char*> member##Pointers;                                           \
        member##Pointers.clear ();                                                                  \
        bool member##Success { true };                                                              \
        for (size_t i { 0 }; (i < result.count) && member##Success; ++i)                            \
        {   const uint8_t* stringData;                                                              \
            size_t stringSize;                                                                      \
            BytesDecoder bytesDecoder { stringData, stringSize };                                   \
            member##Success &= _readFromMessage (message, bytesDecoder);                            \
            member##Pointers.emplace_back (reinterpret_cast<const char*> (stringData));             \
        }                                                                                           \
        result.member = member##Pointers.data ();                                                   \
        if (!member##Success)                                                                       \
        {   result.count = 0;                                                                       \
            result.member = nullptr;                                                                \
            return false;                                                                           \
        }
#define ARA_IPC_DECODE_ADDENDUM_VARIABLE_STRING_ARRAY(member, count)                                \
        if (ARA_IPC_HAS_ADDENDUM_MEMBER (result, member))                                           \
        {   ARA_IPC_DECODE_VARIABLE_STRING_ARRAY (member, count)                                    \
        }

#define ARA_IPC_END_DECODE_STRUCT(StructT, sizeExpr)                                                \
        for (auto offset : _StringMemberOffsets<StructT>::_get ())                                  \
            if (offset < sizeExpr)                                                                  \
                if (const auto stringSize { *reinterpret_cast<size_t*> (structPtr + offset) })      \
                {   const auto stringData { message->readBytes (stringSize) };                      \
                    ARA_INTERNAL_ASSERT (stringData != nullptr);                                    \
                    *reinterpret_cast<const char**> (structPtr + offset) = reinterpret_cast<const char*> (stringData); \
                    if (stringData == nullptr)                                                      \
                        return false;                                                               \
                }                                                                                   \
        return true;                                                                                \
    }                                                                                               \
};
#define ARA_IPC_END_DECODE_FIXED_STRUCT(StructT)                                                    \
        ARA_IPC_END_DECODE_STRUCT (StructT, sizeof (StructT))
#define ARA_IPC_END_DECODE_SIZED_STRUCT(StructT)                                                    \
        ARA_IPC_END_DECODE_STRUCT (StructT, result.structSize)


ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_3 (ARAColor, _4ByteValueMemberOffsets, r, g, b)
ARA_IPC_BEGIN_ENCODE_FIXED_STRUCT (ARAColor)
ARA_IPC_END_ENCODE_FIXED_STRUCT (ARAColor)
ARA_IPC_BEGIN_DECODE_FIXED_STRUCT (ARAColor)
ARA_IPC_END_DECODE_FIXED_STRUCT (ARAColor)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1 (ARADocumentProperties, _StringMemberOffsets, name)

ARA_IPC_BEGIN_ENCODE_SIZED_STRUCT (ARADocumentProperties)
ARA_IPC_END_ENCODE_SIZED_STRUCT (ARADocumentProperties)
ARA_IPC_BEGIN_DECODE_SIZED_STRUCT (ARADocumentProperties)
ARA_IPC_END_DECODE_SIZED_STRUCT (ARADocumentProperties)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1 (ARAMusicalContextProperties, _4ByteValueMemberOffsets, orderIndex)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1 (ARAMusicalContextProperties, _StringMemberOffsets, name)
ARA_IPC_BEGIN_ENCODE_SIZED_STRUCT (ARAMusicalContextProperties)
    ARA_IPC_ENCODE_ADDENDUM_STRUCT_PTR_MEMBER (color)
ARA_IPC_END_ENCODE_SIZED_STRUCT (ARAMusicalContextProperties)
ARA_IPC_BEGIN_DECODE_SIZED_STRUCT (ARAMusicalContextProperties)
    ARA_IPC_DECODE_ADDENDUM_STRUCT_PTR_MEMBER (color)
ARA_IPC_END_DECODE_SIZED_STRUCT (ARAMusicalContextProperties)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1 (ARARegionSequenceProperties, _4ByteValueMemberOffsets, orderIndex)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1 (ARARegionSequenceProperties, _8ByteValueMemberOffsets, musicalContextRef)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2 (ARARegionSequenceProperties, _StringMemberOffsets, name, persistentID)
ARA_IPC_BEGIN_ENCODE_SIZED_STRUCT (ARARegionSequenceProperties)
    ARA_IPC_ENCODE_ADDENDUM_STRUCT_PTR_MEMBER (color)
ARA_IPC_END_ENCODE_SIZED_STRUCT (ARARegionSequenceProperties)
ARA_IPC_BEGIN_DECODE_SIZED_STRUCT (ARARegionSequenceProperties)
    ARA_IPC_DECODE_ADDENDUM_STRUCT_PTR_MEMBER (color)
ARA_IPC_END_DECODE_SIZED_STRUCT (ARARegionSequenceProperties)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_3 (ARAAudioSourceProperties, _4ByteValueMemberOffsets, channelCount, merits64BitSamples, channelArrangementDataType)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2 (ARAAudioSourceProperties, _8ByteValueMemberOffsets, sampleCount, sampleRate)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2 (ARAAudioSourceProperties, _StringMemberOffsets, name, persistentID)
ARA_IPC_BEGIN_ENCODE_SIZED_STRUCT (ARAAudioSourceProperties)
    if (ARA_IPC_HAS_ADDENDUM_MEMBER (value, channelArrangement) && (value.channelArrangement != nullptr))
    {   // \todo potential endianness conversion is missing here -
        //       since the details of the formats are no known at the IPC layer,
        //       this should be added to the ChannelFormat class.
        const auto size { ChannelFormat::getChannelArrangementDataSize (value.channelCount, value.channelArrangementDataType, value.channelArrangement) };
        pendingBytes.emplace_back (reinterpret_cast<const uint8_t*> (value.channelArrangement), size, true);
        *reinterpret_cast<size_t*> (data + offsetof (ARAAudioSourceProperties, channelArrangement)) = size;
    }
ARA_IPC_END_ENCODE_SIZED_STRUCT (ARAAudioSourceProperties)
ARA_IPC_BEGIN_DECODE_SIZED_STRUCT (ARAAudioSourceProperties)
    if (result.structSize > offsetof (ARAAudioSourceProperties, channelArrangement))
    {   if (result.channelArrangementDataType == kARAChannelArrangementUndefined)
        {   result.channelArrangement = nullptr;
        }
        else
        {   if (const auto channelArrangementSize { reinterpret_cast<size_t> (result.channelArrangement) })
            {   const auto channelArrangementData { message->readBytes (channelArrangementSize) };
                ARA_INTERNAL_ASSERT (channelArrangementData != nullptr);
                result.channelArrangement = channelArrangementData;
                if (channelArrangementData == nullptr)
                {   result.channelArrangementDataType = kARAChannelArrangementUndefined;
                    return false;
                }
            }
        }
    }
ARA_IPC_END_DECODE_SIZED_STRUCT (ARAAudioSourceProperties)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2 (ARAAudioModificationProperties, _StringMemberOffsets, name, persistentID)
ARA_IPC_BEGIN_ENCODE_SIZED_STRUCT (ARAAudioModificationProperties)
ARA_IPC_END_ENCODE_SIZED_STRUCT (ARAAudioModificationProperties)
ARA_IPC_BEGIN_DECODE_SIZED_STRUCT (ARAAudioModificationProperties)
ARA_IPC_END_DECODE_SIZED_STRUCT (ARAAudioModificationProperties)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1 (ARAPlaybackRegionProperties, _4ByteValueMemberOffsets, transformationFlags)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_6 (ARAPlaybackRegionProperties, _8ByteValueMemberOffsets, startInModificationTime, durationInModificationTime, startInPlaybackTime, durationInPlaybackTime, musicalContextRef, regionSequenceRef)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1 (ARAPlaybackRegionProperties, _StringMemberOffsets, name)
ARA_IPC_BEGIN_ENCODE_SIZED_STRUCT (ARAPlaybackRegionProperties)
    ARA_IPC_ENCODE_ADDENDUM_STRUCT_PTR_MEMBER (color)
ARA_IPC_END_ENCODE_SIZED_STRUCT (ARAPlaybackRegionProperties)
ARA_IPC_BEGIN_DECODE_SIZED_STRUCT (ARAPlaybackRegionProperties)
    ARA_IPC_DECODE_ADDENDUM_STRUCT_PTR_MEMBER (color)
ARA_IPC_END_DECODE_SIZED_STRUCT (ARAPlaybackRegionProperties)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2 (ARAContentTimeRange, _8ByteValueMemberOffsets, start, duration)
ARA_IPC_BEGIN_ENCODE_FIXED_STRUCT (ARAContentTimeRange)
ARA_IPC_END_ENCODE_FIXED_STRUCT (ARAContentTimeRange)
ARA_IPC_BEGIN_DECODE_FIXED_STRUCT (ARAContentTimeRange)
ARA_IPC_END_DECODE_FIXED_STRUCT (ARAContentTimeRange)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2 (ARAContentTempoEntry, _8ByteValueMemberOffsets, timePosition, quarterPosition)
ARA_IPC_BEGIN_ENCODE_FIXED_STRUCT (ARAContentTempoEntry)
ARA_IPC_END_ENCODE_FIXED_STRUCT (ARAContentTempoEntry)
ARA_IPC_BEGIN_DECODE_FIXED_STRUCT (ARAContentTempoEntry)
ARA_IPC_END_DECODE_FIXED_STRUCT (ARAContentTempoEntry)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2 (ARAContentBarSignature, _4ByteValueMemberOffsets, numerator, denominator)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1 (ARAContentBarSignature, _8ByteValueMemberOffsets, position)
ARA_IPC_BEGIN_ENCODE_FIXED_STRUCT (ARAContentBarSignature)
ARA_IPC_END_ENCODE_FIXED_STRUCT (ARAContentBarSignature)
ARA_IPC_BEGIN_DECODE_FIXED_STRUCT (ARAContentBarSignature)
ARA_IPC_END_DECODE_FIXED_STRUCT (ARAContentBarSignature)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_3 (ARAContentNote, _4ByteValueMemberOffsets, frequency, pitchNumber, volume)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_4 (ARAContentNote, _8ByteValueMemberOffsets, startPosition, attackDuration, noteDuration, signalDuration)
ARA_IPC_BEGIN_ENCODE_FIXED_STRUCT (ARAContentNote)
ARA_IPC_END_ENCODE_FIXED_STRUCT (ARAContentNote)
ARA_IPC_BEGIN_DECODE_FIXED_STRUCT (ARAContentNote)
ARA_IPC_END_DECODE_FIXED_STRUCT (ARAContentNote)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2 (ARAContentTuning, _4ByteValueMemberOffsets, concertPitchFrequency, root)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1 (ARAContentTuning, _StringMemberOffsets, name)
// ARAContentKeySignature::tunings is an array of float, thus copying it is sufficient as long as endianness matches
ARA_IPC_BEGIN_ENCODE_FIXED_STRUCT (ARAContentTuning)
    // \todo add endian swap here if needed
ARA_IPC_END_ENCODE_FIXED_STRUCT (ARAContentTuning)
ARA_IPC_BEGIN_DECODE_FIXED_STRUCT (ARAContentTuning)
ARA_IPC_END_DECODE_FIXED_STRUCT (ARAContentTuning)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1 (ARAContentKeySignature, _4ByteValueMemberOffsets, root)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1 (ARAContentKeySignature, _8ByteValueMemberOffsets, position)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1 (ARAContentKeySignature, _StringMemberOffsets, name)
// ARAContentKeySignature::intervals is an array of bytes, thus simply copying it is sufficient
ARA_IPC_BEGIN_ENCODE_FIXED_STRUCT (ARAContentKeySignature)
ARA_IPC_END_ENCODE_FIXED_STRUCT (ARAContentKeySignature)
ARA_IPC_BEGIN_DECODE_FIXED_STRUCT (ARAContentKeySignature)
ARA_IPC_END_DECODE_FIXED_STRUCT (ARAContentKeySignature)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2 (ARAContentChord, _4ByteValueMemberOffsets, root, bass)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1 (ARAContentChord, _8ByteValueMemberOffsets, position)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1 (ARAContentChord, _StringMemberOffsets, name)
// ARAContentChord::intervals is an array of bytes, thus simply copying it is sufficient
ARA_IPC_BEGIN_ENCODE_FIXED_STRUCT (ARAContentChord)
ARA_IPC_END_ENCODE_FIXED_STRUCT (ARAContentChord)
ARA_IPC_BEGIN_DECODE_FIXED_STRUCT (ARAContentChord)
ARA_IPC_END_DECODE_FIXED_STRUCT (ARAContentChord)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2 (ARAContentLyricsEntry, _4ByteValueMemberOffsets, continuesPreviousWord, phonemesGrade)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2 (ARAContentLyricsEntry, _8ByteValueMemberOffsets, phonemeCount, position)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2 (ARAContentLyricsEntry, _StringMemberOffsets, lyrics, language)
ARA_IPC_BEGIN_ENCODE_FIXED_STRUCT (ARAContentLyricsEntry)
    ARA_IPC_ENCODE_VARIABLE_STRING_ARRAY (phonemes, phonemeCount)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (phonemeOffsets, phonemeCount)
ARA_IPC_END_ENCODE_FIXED_STRUCT (ARAContentLyricsEntry)
ARA_IPC_BEGIN_DECODE_FIXED_STRUCT (ARAContentLyricsEntry)
    ARA_IPC_DECODE_VARIABLE_STRING_ARRAY (phonemes, phonemeCount)
    ARA_IPC_DECODE_VARIABLE_ARRAY (phonemeOffsets, phonemeCount)
ARA_IPC_END_DECODE_FIXED_STRUCT (ARAContentLyricsEntry)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1 (ARARestoreObjectsFilter, _4ByteValueMemberOffsets, documentData)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_3 (ARARestoreObjectsFilter, _8ByteValueMemberOffsets, audioSourceIDsCount, audioModificationIDsCount, regionSequenceIDsCount)
ARA_IPC_BEGIN_ENCODE_SIZED_STRUCT (ARARestoreObjectsFilter)
    ARA_IPC_ENCODE_VARIABLE_STRING_ARRAY (audioSourceArchiveIDs, audioSourceIDsCount)
    if (value.audioSourceCurrentIDs != nullptr)
    {   ARA_IPC_ENCODE_VARIABLE_STRING_ARRAY (audioSourceCurrentIDs, audioSourceIDsCount)
    }
    ARA_IPC_ENCODE_VARIABLE_STRING_ARRAY (audioModificationArchiveIDs, audioModificationIDsCount)
    if (value.audioModificationCurrentIDs != nullptr)
    {   ARA_IPC_ENCODE_VARIABLE_STRING_ARRAY (audioModificationCurrentIDs, audioModificationIDsCount)
    }
    ARA_IPC_ENCODE_ADDENDUM_VARIABLE_STRING_ARRAY (regionSequenceArchiveIDs, regionSequenceIDsCount)
    if (value.regionSequenceCurrentIDs != nullptr)
    {   ARA_IPC_ENCODE_ADDENDUM_VARIABLE_ARRAY (regionSequenceCurrentIDs, regionSequenceIDsCount)
    }
ARA_IPC_END_ENCODE_SIZED_STRUCT (ARARestoreObjectsFilter)
ARA_IPC_BEGIN_DECODE_SIZED_STRUCT (ARARestoreObjectsFilter)
    ARA_IPC_DECODE_VARIABLE_STRING_ARRAY (audioSourceArchiveIDs, audioSourceIDsCount)
    if (result.audioSourceCurrentIDs != nullptr)
    {   ARA_IPC_DECODE_VARIABLE_STRING_ARRAY (audioSourceCurrentIDs, audioSourceIDsCount)
    }
    ARA_IPC_DECODE_VARIABLE_STRING_ARRAY (audioModificationArchiveIDs, audioModificationIDsCount)
    if (result.audioModificationCurrentIDs != nullptr)
    {   ARA_IPC_DECODE_VARIABLE_STRING_ARRAY (audioModificationCurrentIDs, audioModificationIDsCount)
    }
    ARA_IPC_DECODE_ADDENDUM_VARIABLE_STRING_ARRAY (regionSequenceArchiveIDs, regionSequenceIDsCount)
    if (result.regionSequenceCurrentIDs != nullptr)
    {   ARA_IPC_DECODE_ADDENDUM_VARIABLE_ARRAY (regionSequenceCurrentIDs, regionSequenceIDsCount)
    }
ARA_IPC_END_DECODE_SIZED_STRUCT (ARARestoreObjectsFilter)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1 (ARAStoreObjectsFilter, _4ByteValueMemberOffsets, documentData)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_3 (ARAStoreObjectsFilter, _8ByteValueMemberOffsets, audioSourceRefsCount, audioModificationRefsCount, regionSequenceRefsCount)
 ARA_IPC_BEGIN_ENCODE_SIZED_STRUCT (ARAStoreObjectsFilter)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (audioSourceRefs, audioSourceRefsCount)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (audioModificationRefs, audioModificationRefsCount)
    ARA_IPC_ENCODE_ADDENDUM_VARIABLE_ARRAY (regionSequenceRefs, regionSequenceRefsCount)
ARA_IPC_END_ENCODE_SIZED_STRUCT (ARAStoreObjectsFilter)
ARA_IPC_BEGIN_DECODE_SIZED_STRUCT (ARAStoreObjectsFilter)
    ARA_IPC_DECODE_VARIABLE_ARRAY (audioSourceRefs, audioSourceRefsCount)
    ARA_IPC_DECODE_VARIABLE_ARRAY (audioModificationRefs, audioModificationRefsCount)
    ARA_IPC_DECODE_ADDENDUM_VARIABLE_ARRAY (regionSequenceRefs, regionSequenceRefsCount)
ARA_IPC_END_DECODE_SIZED_STRUCT (ARAStoreObjectsFilter)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2 (ARAProcessingAlgorithmProperties, _StringMemberOffsets, persistentID, name)
ARA_IPC_BEGIN_ENCODE_SIZED_STRUCT (ARAProcessingAlgorithmProperties)
ARA_IPC_END_ENCODE_SIZED_STRUCT (ARAProcessingAlgorithmProperties)
ARA_IPC_BEGIN_DECODE_SIZED_STRUCT (ARAProcessingAlgorithmProperties)
ARA_IPC_END_DECODE_SIZED_STRUCT (ARAProcessingAlgorithmProperties)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2 (ARAViewSelection, _8ByteValueMemberOffsets, playbackRegionRefsCount, regionSequenceRefsCount)
ARA_IPC_BEGIN_ENCODE_SIZED_STRUCT (ARAViewSelection)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (playbackRegionRefs, playbackRegionRefsCount)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (regionSequenceRefs, regionSequenceRefsCount)
    ARA_IPC_ENCODE_STRUCT_PTR_MEMBER (timeRange)
ARA_IPC_END_ENCODE_SIZED_STRUCT (ARAViewSelection)
ARA_IPC_BEGIN_DECODE_SIZED_STRUCT (ARAViewSelection)
    ARA_IPC_DECODE_VARIABLE_ARRAY (playbackRegionRefs, playbackRegionRefsCount)
    ARA_IPC_DECODE_VARIABLE_ARRAY (regionSequenceRefs, regionSequenceRefsCount)
    ARA_IPC_DECODE_STRUCT_PTR_MEMBER (timeRange)
ARA_IPC_END_DECODE_SIZED_STRUCT (ARAViewSelection)

ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_7 (ARAFactory, _4ByteValueMemberOffsets, lowestSupportedApiGeneration, highestSupportedApiGeneration,
                                     supportedPlaybackTransformationFlags, supportsStoringAudioFileChunks,
                                     supportsSampleBasedAudioSources, supportsContentOnlyAudioSources, requiresPresetAudioSources)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_6 (ARAFactory, _StringMemberOffsets, factoryID, plugInName, manufacturerName,
                                     informationURL, version, documentArchiveID)
ARA_IPC_BEGIN_ENCODE_SIZED_STRUCT (ARAFactory)
    ARA_IPC_ENCODE_VARIABLE_STRING_ARRAY (compatibleDocumentArchiveIDs, compatibleDocumentArchiveIDsCount)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (analyzeableContentTypes, analyzeableContentTypesCount)
ARA_IPC_END_ENCODE_SIZED_STRUCT (ARAFactory)
ARA_IPC_BEGIN_DECODE_SIZED_STRUCT (ARAFactory)
    result.initializeARAWithConfiguration = nullptr;
    result.uninitializeARA = nullptr;
    result.createDocumentControllerWithDocument = nullptr;
    ARA_IPC_DECODE_VARIABLE_STRING_ARRAY (compatibleDocumentArchiveIDs, compatibleDocumentArchiveIDsCount)
    ARA_IPC_DECODE_VARIABLE_ARRAY (analyzeableContentTypes, analyzeableContentTypesCount)
ARA_IPC_END_DECODE_SIZED_STRUCT (ARAFactory)

// ARADocumentControllerInterface::storeAudioSourceToAudioFileChunk() must return the documentArchiveID and the
// openAutomatically flag in addition to the return value, we need a special struct to encode this through IPC.
struct StoreAudioSourceToAudioFileChunkReply
{
    ARAPersistentID documentArchiveID;
    ARABool openAutomatically;
    ARABool result;
};
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2 (StoreAudioSourceToAudioFileChunkReply, _4ByteValueMemberOffsets, openAutomatically, result)
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1 (StoreAudioSourceToAudioFileChunkReply, _StringMemberOffsets, documentArchiveID)
ARA_IPC_BEGIN_ENCODE_FIXED_STRUCT (StoreAudioSourceToAudioFileChunkReply)
ARA_IPC_END_ENCODE_FIXED_STRUCT (StoreAudioSourceToAudioFileChunkReply)
ARA_IPC_BEGIN_DECODE_FIXED_STRUCT (StoreAudioSourceToAudioFileChunkReply)
ARA_IPC_END_DECODE_FIXED_STRUCT (StoreAudioSourceToAudioFileChunkReply)

// ARADocumentControllerInterface::getPlaybackRegionHeadAndTailTime() must return both head- and tailtime.
struct GetPlaybackRegionHeadAndTailTimeReply
{
    ARATimeDuration headTime;
    ARATimeDuration tailTime;
};
ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2 (GetPlaybackRegionHeadAndTailTimeReply, _8ByteValueMemberOffsets, headTime, tailTime)
ARA_IPC_BEGIN_ENCODE_FIXED_STRUCT (GetPlaybackRegionHeadAndTailTimeReply)
ARA_IPC_END_ENCODE_FIXED_STRUCT (GetPlaybackRegionHeadAndTailTimeReply)
ARA_IPC_BEGIN_DECODE_FIXED_STRUCT (GetPlaybackRegionHeadAndTailTimeReply)
ARA_IPC_END_DECODE_FIXED_STRUCT (GetPlaybackRegionHeadAndTailTimeReply)


#undef ARA_IPC_HAS_ADDENDUM_MEMBER
#undef ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_1
#undef ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_2
#undef ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_3
#undef ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_4
#undef ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_5
#undef ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_6
#undef ARA_IPC_SPECIALIZE_MEMBER_OFFSETS_7
#undef ARA_IPC_BEGIN_ENCODE_STRUCT
#undef ARA_IPC_BEGIN_ENCODE_FIXED_STRUCT
#undef ARA_IPC_BEGIN_ENCODE_SIZED_STRUCT
#undef ARA_IPC_ENCODE_STRUCT_PTR_MEMBER
#undef ARA_IPC_ENCODE_ADDENDUM_STRUCT_PTR_MEMBER
#undef ARA_IPC_ENCODE_VARIABLE_ARRAY
#undef ARA_IPC_ENCODE_ADDENDUM_VARIABLE_ARRAY
#undef ARA_IPC_ENCODE_VARIABLE_STRING_ARRAY
#undef ARA_IPC_END_ENCODE_STRUCT
#undef ARA_IPC_END_ENCODE_FIXED_STRUCT
#undef ARA_IPC_END_ENCODE_SIZED_STRUCT
#undef ARA_IPC_BEGIN_DECODE_STRUCT
#undef ARA_IPC_BEGIN_DECODE_FIXED_STRUCT
#undef ARA_IPC_BEGIN_DECODE_SIZED_STRUCT
#undef ARA_IPC_DECODE_STRUCT_PTR_MEMBER
#undef ARA_IPC_DECODE_ADDENDUM_STRUCT_PTR_MEMBER
#undef ARA_IPC_DECODE_VARIABLE_ARRAY
#undef ARA_IPC_DECODE_ADDENDUM_VARIABLE_ARRAY
#undef ARA_IPC_END_DECODE_STRUCT
#undef ARA_IPC_END_DECODE_FIXED_STRUCT
#undef ARA_IPC_END_DECODE_SIZED_STRUCT


// actual definitions of wrapper functions to implicitly deduce _ValueEn-/Decoder<> -
// they are forward-declared above, before _ValueEn-/Decoder<> are defined
template <typename ValueT>
inline void _encodeAndAppend (OutgoingMessage* message, const ValueT& argValue)
{
    return _ValueEncoder<ValueT>::encodeAndAppend (message, argValue);
}
template <typename ValueT>
inline bool _readAndDecode (ValueT& result, const IncomingMessage* message)
{
    return _ValueDecoder<ValueT>::readAndDecode (result, message);
}


//------------------------------------------------------------------------------

// private helper for ARA_IPC_METHOD_ID
template<typename StructT>
constexpr MessageID _getInterfaceID ();
// host interface specializations
template<>
constexpr MessageID _getInterfaceID<ARAAudioAccessControllerInterface> () { return 1; }
template<>
constexpr MessageID _getInterfaceID<ARAArchivingControllerInterface> () { return 2; }
template<>
constexpr MessageID _getInterfaceID<ARAContentAccessControllerInterface> () { return 3; }
template<>
constexpr MessageID _getInterfaceID<ARAModelUpdateControllerInterface> () { return 4; }
template<>
constexpr MessageID _getInterfaceID<ARAPlaybackControllerInterface> () { return 5; }
// plug-in interface specializations
template<>
constexpr MessageID _getInterfaceID<ARADocumentControllerInterface> () { return 1; }
template<>
constexpr MessageID _getInterfaceID<ARAPlaybackRendererInterface> () { return 2; }
template<>
constexpr MessageID _getInterfaceID<ARAEditorRendererInterface> () { return 3; }
template<>
constexpr MessageID _getInterfaceID<ARAEditorViewInterface> () { return 4; }


//------------------------------------------------------------------------------
// actual client API
//------------------------------------------------------------------------------


// helper class wrapping MessageID to prevent implicit conversions to/from
// MessageID, so that they can be identified reliably in call signatures
// this is important e.g. for the various forms of RemoteCaller::remoteCall ().
class MethodID
{
private:
    constexpr MethodID (MessageID messageID) : _id { messageID } {}

public:
    template<MessageID interfaceID, size_t offset>
    static inline constexpr MethodID createWithARAInterfaceIDAndOffset ()
    {
        static_assert (offset > 0, "offset 0 is never a valid function pointer");
        static_assert ((interfaceID < 8), "currently using only 3 bits for interface ID");
#if defined (__i386__) || defined (_M_IX86)
        static_assert ((sizeof (void*) == 4), "compiler settings imply 32 bit pointers");
        static_assert (((offset & 0x3FFFFFF4) == offset), "offset is misaligned or too large");
        // lower 2 bits of offset are 0 due to alignment, must shift 1 bit to store interface ID
        constexpr auto messageID { (offset << 1) + interfaceID };
#else
        static_assert ((sizeof (void*) == 8), "assuming 64 bit pointers per default");
        static_assert (((offset & 0x7FFFFFF8) == offset), "offset is misaligned or too large");
        // lower 3 bits of offset are 0 due to alignment, can be used to store interface ID
        constexpr auto messageID { offset + interfaceID };
#endif
        static_assert (!isCustomMessageID (messageID), "must not be in custom message range");
        return messageID;
    }

    template<size_t index>
    static inline constexpr MethodID createWithARAGlobalMessageIndex ()
    {
        return createWithARAInterfaceIDAndOffset <0, (index << 3)> ();
    }

    template<MessageID messageID>
    static inline constexpr MethodID createWithCustomMessageID ()
    {
        static_assert (isCustomMessageID (messageID), "must not be in reserved message range");
        return messageID;
    }

    static inline constexpr bool isCustomMessageID (MessageID messageID)
    {
        return (messageID < kReservedMessageIDRangeStart) || (kReservedMessageIDRangeEnd <= messageID);
    }

    constexpr MessageID getMessageID () const { return _id; }

private:
    const MessageID _id;
};

inline bool operator== (const MessageID messageID, const MethodID methodID)
{
    return (messageID == methodID.getMessageID ());
}
inline bool operator== (const MethodID methodID, const MessageID messageID)
{
    return (messageID == methodID.getMessageID ());
}
inline bool operator!= (const MessageID messageID, const MethodID methodID)
{
    return (messageID != methodID.getMessageID ());
}
inline bool operator!= (const MethodID methodID, const MessageID messageID)
{
    return (messageID != methodID.getMessageID ());
}


// create a MethodID for a given host-side or plug-in-side ARA method
#define ARA_IPC_METHOD_ID(StructT, member) MethodID::createWithARAInterfaceIDAndOffset <_getInterfaceID<StructT> (), offsetof (StructT, member)> ()


// "global" messages for startup and teardown that are not calculated based on interface structs
inline constexpr auto kGetFactoriesCountMethodID { MethodID::createWithARAGlobalMessageIndex<1> () };
inline constexpr auto kGetFactoryMethodID { MethodID::createWithARAGlobalMessageIndex<2> () };
inline constexpr auto kInitializeARAMethodID { MethodID::createWithARAGlobalMessageIndex<3> () };
inline constexpr auto kCreateDocumentControllerMethodID { MethodID::createWithARAGlobalMessageIndex<4> () };
inline constexpr auto kBindToDocumentControllerMethodID { MethodID::createWithARAGlobalMessageIndex<5> () };
inline constexpr auto kCleanupBindingMethodID { MethodID::createWithARAGlobalMessageIndex<6> () };
inline constexpr auto kUninitializeARAMethodID { MethodID::createWithARAGlobalMessageIndex<7> () };


// private helpers for decodeHost/PlugInMessageID()
#define ARA_IPC_GLOBAL_MESSAGE_CASE(methodID) \
        case methodID.getMessageID (): return #methodID;

#define ARA_IPC_INTERFACE_MESSAGE_CASE(StructT, member) \
        case ARA_IPC_METHOD_ID (StructT, member).getMessageID (): return (omitInterfaceName) ? #member : #StructT "::" #member;

// for debugging: translate IDs of messages sent by the host back to human-readable text
inline const char* decodeHostMessageID (const MessageID messageID, const bool omitInterfaceName = true)
{
    switch (messageID)
    {
        ARA_IPC_GLOBAL_MESSAGE_CASE (kGetFactoriesCountMethodID);
        ARA_IPC_GLOBAL_MESSAGE_CASE (kGetFactoryMethodID);
        ARA_IPC_GLOBAL_MESSAGE_CASE (kInitializeARAMethodID);
        ARA_IPC_GLOBAL_MESSAGE_CASE (kCreateDocumentControllerMethodID);
        ARA_IPC_GLOBAL_MESSAGE_CASE (kBindToDocumentControllerMethodID);
        ARA_IPC_GLOBAL_MESSAGE_CASE (kCleanupBindingMethodID);
        ARA_IPC_GLOBAL_MESSAGE_CASE (kUninitializeARAMethodID);

        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, destroyDocumentController)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, getFactory)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, beginEditing)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, endEditing)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, notifyModelUpdates)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, beginRestoringDocumentFromArchive)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, endRestoringDocumentFromArchive)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, storeDocumentToArchive)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, updateDocumentProperties)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, createMusicalContext)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, updateMusicalContextProperties)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, updateMusicalContextContent)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, destroyMusicalContext)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, createAudioSource)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, updateAudioSourceProperties)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, updateAudioSourceContent)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, enableAudioSourceSamplesAccess)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, deactivateAudioSourceForUndoHistory)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, destroyAudioSource)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, createAudioModification)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, cloneAudioModification)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, updateAudioModificationProperties)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, deactivateAudioModificationForUndoHistory)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, destroyAudioModification)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, createPlaybackRegion)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, updatePlaybackRegionProperties)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, destroyPlaybackRegion)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, isAudioSourceContentAvailable)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, isAudioSourceContentAnalysisIncomplete)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, requestAudioSourceContentAnalysis)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, getAudioSourceContentGrade)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, createAudioSourceContentReader)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, isAudioModificationContentAvailable)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, getAudioModificationContentGrade)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, createAudioModificationContentReader)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, isPlaybackRegionContentAvailable)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, getPlaybackRegionContentGrade)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, createPlaybackRegionContentReader)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, getContentReaderEventCount)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, getContentReaderDataForEvent)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, destroyContentReader)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, createRegionSequence)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, updateRegionSequenceProperties)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, destroyRegionSequence)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, getPlaybackRegionHeadAndTailTime)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, restoreObjectsFromArchive)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, storeObjectsToArchive)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, getProcessingAlgorithmsCount)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, getProcessingAlgorithmProperties)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, getProcessingAlgorithmForAudioSource)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, requestProcessingAlgorithmForAudioSource)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, isLicensedForCapabilities)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, storeAudioSourceToAudioFileChunk)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, isAudioModificationPreservingAudioSourceSignal)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARADocumentControllerInterface, isPlaybackRegionPreservingAudioSourceSignal)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAPlaybackRendererInterface, addPlaybackRegion)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAPlaybackRendererInterface, removePlaybackRegion)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAEditorRendererInterface, addPlaybackRegion)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAEditorRendererInterface, removePlaybackRegion)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAEditorRendererInterface, addRegionSequence)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAEditorRendererInterface, removeRegionSequence)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAEditorViewInterface, notifySelection)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAEditorViewInterface, notifyHideRegionSequences)
    }
    if (MethodID::isCustomMessageID (messageID))
        return "custom message";
    ARA_INTERNAL_ASSERT (false);
    return "unknown message";
}

// for debugging: translate IDs of messages sent by the host back to human-readable text
inline const char* decodePlugInMessageID (const MessageID messageID, const bool omitInterfaceName = true)
{
    switch (messageID)
    {
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAAudioAccessControllerInterface, createAudioReaderForSource)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAAudioAccessControllerInterface, readAudioSamples)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAAudioAccessControllerInterface, destroyAudioReader)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAArchivingControllerInterface, getArchiveSize)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAArchivingControllerInterface, readBytesFromArchive)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAArchivingControllerInterface, writeBytesToArchive)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAArchivingControllerInterface, notifyDocumentArchivingProgress)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAArchivingControllerInterface, notifyDocumentUnarchivingProgress)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAArchivingControllerInterface, getDocumentArchiveID)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAContentAccessControllerInterface, isMusicalContextContentAvailable)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAContentAccessControllerInterface, getMusicalContextContentGrade)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAContentAccessControllerInterface, createMusicalContextContentReader)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAContentAccessControllerInterface, isAudioSourceContentAvailable)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAContentAccessControllerInterface, getAudioSourceContentGrade)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAContentAccessControllerInterface, createAudioSourceContentReader)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAContentAccessControllerInterface, getContentReaderEventCount)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAContentAccessControllerInterface, getContentReaderDataForEvent)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAContentAccessControllerInterface, destroyContentReader)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAModelUpdateControllerInterface, notifyAudioSourceAnalysisProgress)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAModelUpdateControllerInterface, notifyAudioSourceContentChanged)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAModelUpdateControllerInterface, notifyAudioModificationContentChanged)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAModelUpdateControllerInterface, notifyPlaybackRegionContentChanged)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAModelUpdateControllerInterface, notifyDocumentDataChanged)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAModelUpdateControllerInterface, notifyRegionSequenceDataChanged)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAPlaybackControllerInterface, requestStartPlayback)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAPlaybackControllerInterface, requestStopPlayback)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAPlaybackControllerInterface, requestSetPlaybackPosition)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAPlaybackControllerInterface, requestSetCycleRange)
        ARA_IPC_INTERFACE_MESSAGE_CASE (ARAPlaybackControllerInterface, requestEnableCycle)
    }
    if (MethodID::isCustomMessageID (messageID))
        return "custom message";
    ARA_INTERNAL_ASSERT (false);
    return "unknown message";
}

#undef ARA_IPC_GLOBAL_MESSAGE_CASE
#undef ARA_IPC_INTERFACE_MESSAGE_CASE

// helper template to identify pointers to ARA structs in message arguments
template<typename ArgT>
inline constexpr auto _IsStructPointerArg = !_IsRefType<ArgT> && std::is_pointer_v<ArgT> && !std::is_same_v<ArgT, const char*>;

// helper template for encodeMessage() to replace potential null pointers with optionals
template<typename ArgT, std::enable_if_t<!_IsStructPointerArg<ArgT>, int> = 0>
inline void _encodeAndAppendHelper (OutgoingMessage* message, const ArgT& argValue)
{
    _encodeAndAppend (message, argValue);
}
template<typename ArgT, std::enable_if_t<_IsStructPointerArg<ArgT>, int> = 0>
inline void _encodeAndAppendHelper (OutgoingMessage* message, const ArgT& argValue)
{
    _encodeAndAppend (message, (argValue) ? std::make_optional (*argValue) : std::nullopt);
}
inline void _encodeAndAppendHelper (OutgoingMessage* message, const std::nullptr_t& /*argValue*/)
{
    // all optional types encode the value-not-contained case the same, so we can just pick any type here
    _encodeAndAppend (message, std::optional<size_t> ());
}


//------------------------------------------------------------------------------
// Top-level en- and decoding methods
//------------------------------------------------------------------------------

template<typename... Args>
inline void encodeMessage (OutgoingMessage* message, const Args &... args)
{
    ARA_INTERNAL_ASSERT (message != nullptr);
    ((_encodeAndAppendHelper (message, args)), ...);
}

template<typename... Args>
inline bool decodeMessage (const IncomingMessage* message, Args &... args)
{
    ARA_INTERNAL_ASSERT (message != nullptr);
    bool success { true };
    ((success &= _readAndDecode (args, message)), ...);
    return success;
}


//------------------------------------------------------------------------------
// support for content readers
//------------------------------------------------------------------------------

inline void encodeContentEvent (OutgoingMessage* message, const ARAContentType type, const void* eventData)
{
    switch (type)
    {
        case kARAContentTypeNotes:          encodeMessage (message, *static_cast<const ARAContentNote*> (eventData)); break;
        case kARAContentTypeTempoEntries:   encodeMessage (message, *static_cast<const ARAContentTempoEntry*> (eventData)); break;
        case kARAContentTypeBarSignatures:  encodeMessage (message, *static_cast<const ARAContentBarSignature*> (eventData)); break;
        case kARAContentTypeStaticTuning:   encodeMessage (message, *static_cast<const ARAContentTuning*> (eventData)); break;
        case kARAContentTypeKeySignatures:  encodeMessage (message, *static_cast<const ARAContentKeySignature*> (eventData)); break;
        case kARAContentTypeSheetChords:    encodeMessage (message, *static_cast<const ARAContentChord*> (eventData)); break;
        case kARAContentTypeLyricEntries:   encodeMessage (message, *static_cast<const ARAContentLyricsEntry*> (eventData)); break;
        default:                            ARA_INTERNAL_ASSERT (false && "content type not implemented yet"); break;
    }
}

class ContentEventDecoder
{
public:
    ContentEventDecoder (ARAContentType type)
    : _decoderFunction { _getDecoderFunctionForContentType (type) },
      _contentString { _findStringMember (type) }
    {}

    const void* decode (const IncomingMessage* message)
    {
        (this->*_decoderFunction) (message);
        return &_eventStorage;
    }

private:
    using _DecoderFunction = void (ContentEventDecoder::*) (const IncomingMessage*);

    template<typename ContentT>
    void _decodeContentEvent (const IncomingMessage* message)
    {
        decodeMessage (message, *reinterpret_cast<ContentT*> (&this->_eventStorage));
        if (this->_contentString != nullptr)
        {
            this->_stringStorage.assign (*this->_contentString);
            *this->_contentString = this->_stringStorage.c_str ();
        }
    }

    static inline constexpr _DecoderFunction _getDecoderFunctionForContentType (const ARAContentType type)
    {
        switch (type)
        {
            case kARAContentTypeNotes: return &ContentEventDecoder::_decodeContentEvent<ContentTypeMapper<kARAContentTypeNotes>::DataType>;
            case kARAContentTypeTempoEntries: return &ContentEventDecoder::_decodeContentEvent<ContentTypeMapper<kARAContentTypeTempoEntries>::DataType>;
            case kARAContentTypeBarSignatures: return &ContentEventDecoder::_decodeContentEvent<ContentTypeMapper<kARAContentTypeBarSignatures>::DataType>;
            case kARAContentTypeStaticTuning: return &ContentEventDecoder::_decodeContentEvent<ContentTypeMapper<kARAContentTypeStaticTuning>::DataType>;
            case kARAContentTypeKeySignatures: return &ContentEventDecoder::_decodeContentEvent<ContentTypeMapper<kARAContentTypeKeySignatures>::DataType>;
            case kARAContentTypeSheetChords: return &ContentEventDecoder::_decodeContentEvent<ContentTypeMapper<kARAContentTypeSheetChords>::DataType>;
            case kARAContentTypeLyricEntries: return &ContentEventDecoder::_decodeContentEvent<ContentTypeMapper<kARAContentTypeLyricEntries>::DataType>;
            default: ARA_INTERNAL_ASSERT (false); return nullptr;
        }
    }

    static inline constexpr size_t _getStringMemberOffsetForContentType (const ARAContentType type)
    {
        switch (type)
        {
            case kARAContentTypeStaticTuning: return offsetof (ARAContentTuning, name);
            case kARAContentTypeKeySignatures: return offsetof (ARAContentKeySignature, name);
            case kARAContentTypeSheetChords: return offsetof (ARAContentChord, name);
            default: return 0;
        }
    }

    inline ARAUtf8String* _findStringMember (const ARAContentType type)
    {
        const auto offset { ContentEventDecoder::_getStringMemberOffsetForContentType (type) };
        if (offset == 0)
            return nullptr;
        return reinterpret_cast<ARAUtf8String*> (reinterpret_cast<uint8_t *> (&this->_eventStorage) + offset);
    }

private:
    _DecoderFunction const _decoderFunction;    // instead of performing the switch (type) for each event,
                                                // we select a templated member function in the c'tor
    union
    {
        ARAContentTempoEntry _tempoEntry;
        ARAContentBarSignature _barSignature;
        ARAContentNote _note;
        ARAContentTuning _tuning;
        ARAContentKeySignature _keySignature;
        ARAContentChord _chord;
        ARAContentLyricsEntry _lyricsEntry;
    } _eventStorage {};

    ARAUtf8String* _contentString {};
    std::string _stringStorage {};

    ARA_DISABLE_COPY_AND_MOVE (ContentEventDecoder)
};

//------------------------------------------------------------------------------
// implementation helpers
//------------------------------------------------------------------------------

// helper base class to create and send messages, decoding reply if applicable
// It's possible to specify a CustomDecodeFunction as reply type to access an un-decoded reply
// if needed (e.g. to deal with memory ownership issue).
class RemoteCaller
{
public:
    using CustomDecodeFunction = std::function<void (const IncomingMessage* message)>;

    explicit RemoteCaller (Connection* connection) noexcept
    : _connection { connection }
    {}

    template<typename... Args>
    inline void remoteCall (const MethodID methodID, const Args &... args) const
    {
        _connection->sendMessage (methodID.getMessageID (),
                                  [&] (OutgoingMessage* message)
                                  {
                                      ARA_INTERNAL_ASSERT (message);
                                      encodeMessage (message, args...);
                                  });
    }

    template<typename RetT, typename... Args>
    inline void remoteCall (RetT& result, const MethodID methodID, const Args &... args) const
    {
        _connection->sendMessage (methodID.getMessageID (),
                                  [&] (OutgoingMessage* message)
                                  {
                                      ARA_INTERNAL_ASSERT (message);
                                      encodeMessage (message, args...);
                                  },
                                  [&result] (const IncomingMessage* message)
                                  {
                                      ARA_INTERNAL_ASSERT (message);
                                      decodeMessage (message, result);
                                  });
    }

    template<typename... Args>
    inline void remoteCall (CustomDecodeFunction& decodeFunction, const MethodID methodID, const Args &... args) const
    {
        _connection->sendMessage (methodID.getMessageID (),
                                  [&] (OutgoingMessage* message)
                                  {
                                      ARA_INTERNAL_ASSERT (message);
                                      encodeMessage (message, args...);
                                  },
                                  [&decodeFunction] (const IncomingMessage* message)
                                  {
                                      ARA_INTERNAL_ASSERT (message);
                                      decodeFunction (message);
                                  });
    }

    bool receiverEndiannessMatches () const { return _connection->receiverEndiannessMatches (); }

    Connection* getConnection () const { return _connection; }

private:
    Connection* const _connection;
};


}   // namespace IPC
}   // namespace ARA

//! @} ARA_Library_IPC


#endif // ARA_ENABLE_IPC

#endif // ARAIPCEncoding_h
