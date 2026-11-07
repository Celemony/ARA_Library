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
// private primitive wrappers for MessageEn-/Decoder C API
//------------------------------------------------------------------------------


// primitives for appending an argument to a message
inline void _appendToMessage (MessageEncoder* encoder, const int32_t argValue)
{
    encoder->appendInt32 (argValue);
}
inline void _appendToMessage (MessageEncoder* encoder, const int64_t argValue)
{
    encoder->appendInt64 (argValue);
}
inline void _appendToMessage (MessageEncoder* encoder, const size_t argValue)
{
    encoder->appendSize (argValue);
}
inline void _appendToMessage (MessageEncoder* encoder, const float argValue)
{
    encoder->appendFloat (argValue);
}
inline void _appendToMessage (MessageEncoder* encoder, const double argValue)
{
    encoder->appendDouble (argValue);
}
inline void _appendToMessage (MessageEncoder* encoder, const char* const argValue)
{
    encoder->appendString (argValue);
}
inline void _appendToMessage (MessageEncoder* encoder, const BytesEncoder& argValue)
{
    encoder->appendBytes (argValue.bytes, argValue.size, argValue.copy);
}

// primitives for reading an (optional) argument from a message
inline bool _readFromMessage (const MessageDecoder* decoder, int32_t& argValue)
{
    return decoder->readInt32 (argValue);
}
inline bool _readFromMessage (const MessageDecoder* decoder, int64_t& argValue)
{
    return decoder->readInt64 (argValue);
}
inline bool _readFromMessage (const MessageDecoder* decoder, size_t& argValue)
{
    return decoder->readSize (argValue);
}
inline bool _readFromMessage (const MessageDecoder* decoder, float& argValue)
{
    return decoder->readFloat (argValue);
}
inline bool _readFromMessage (const MessageDecoder* decoder, double& argValue)
{
    return decoder->readDouble (argValue);
}
inline bool _readFromMessage (const MessageDecoder* decoder, const char*& argValue)
{
    return decoder->readString (argValue);
}
inline bool _readFromMessage (const MessageDecoder* decoder, BytesDecoder& argValue)
{
    const uint8_t* receivedBytes;
    size_t receivedSize;
    if (!decoder->readBytes (receivedBytes, receivedSize))
        return false;
    return argValue (receivedBytes, receivedSize);
}


//------------------------------------------------------------------------------
// overloads of the IPCMessageEn-/Decoder primitives for types that can be
// directly mapped to a primitive type
//------------------------------------------------------------------------------


// templated overloads of the IPCMessageEn-/Decoder primitives for ARA (host) ref types,
// which are stored as size_t
template<typename T, std::enable_if_t<_IsRefType<T>, bool> = true>
inline void _appendToMessage (MessageEncoder* encoder, const T argValue)
{
    encoder->appendSize (reinterpret_cast<size_t> (argValue));
}
template<typename T, std::enable_if_t<_IsRefType<T>, bool> = true>
inline bool _readFromMessage (const MessageDecoder* decoder, T& argValue)
{
    // \todo is there a safe/proper way across all compilers for this cast to avoid the copy?
//  return decoder.readSize (argKey, *reinterpret_cast<size_t*> (&argValue));
    size_t tmp;
    const auto success { decoder->readSize (tmp) };
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
inline void _encodeAndAppend (MessageEncoder* encoder, const ValueT& argValue);
template <typename ValueT>
inline bool _readAndDecode (ValueT& result, const MessageDecoder* decoder);


// primary templates for basic types (numbers, strings, (host)refs and raw bytes)
template<typename ValueT>
struct _ValueEncoder
{
    static inline void encodeAndAppend (MessageEncoder* encoder, const ValueT& argValue)
    {
        _appendToMessage (encoder, argValue);
    }
};
template<typename ValueT>
struct _ValueDecoder
{
    static inline bool readAndDecode (ValueT& result, const MessageDecoder* decoder)
    {
        return _readFromMessage (decoder, result);
    }
};


// common base classes for en-/decoding compound types (arrays, structs) via nested messages,
// providing the generic encode&append/read&decode calls
template<typename ValueT>
struct _CompoundValueEncoderBase
{
    static inline void encodeAndAppend (MessageEncoder* encoder, const ValueT& argValue)
    {
        _ValueEncoder<ValueT>::encode (encoder, argValue);
    }
};
template<typename ValueT>
struct _CompoundValueDecoderBase
{
    static inline bool readAndDecode (ValueT& result, const MessageDecoder* decoder)
    {
        return _ValueDecoder<ValueT>::decode (result, decoder);
    }
};


// specialization for encoding arrays (variable or fixed size)
template<typename ElementT>
struct _ValueEncoder<ArrayArgument<ElementT>> : public _CompoundValueEncoderBase<ArrayArgument<ElementT>>
{
    static inline void encode (MessageEncoder* encoder, const ArrayArgument<ElementT>& value)
    {
        ARA_INTERNAL_ASSERT ((value.count == 0) || (value.elements != nullptr));
        _encodeAndAppend (encoder, value.count);
        for (size_t i { 0 }; i < value.count; ++i)
            _encodeAndAppend (encoder, value.elements[i]);
    }
};

// specialization for decoding fixed-size arrays
template<typename ElementT>
struct _ValueDecoder<ArrayArgument<ElementT>> : public _CompoundValueDecoderBase<ArrayArgument<ElementT>>
{
    static inline bool decode (ArrayArgument<ElementT>& result, const MessageDecoder* decoder)
    {
        bool success { true };
        size_t count;
        success &= _readAndDecode (count, decoder);
        success &= (count == result.count);
        if (count > result.count)
            count = result.count;

        for (size_t i { 0 }; i < count; ++i)
            success &= _readAndDecode (result.elements[i], decoder);
        return success;
    }
};

// specialization for decoding variable arrays
template<typename ElementT>
struct _ValueDecoder<std::vector<ElementT>> : public _CompoundValueDecoderBase<std::vector<ElementT>>
{
    static inline bool decode (std::vector<ElementT>& result, const MessageDecoder* decoder)
    {
        bool success { true };
        size_t count;
        success &= _readAndDecode (count, decoder);
        result.resize (count);
        for (size_t i { 0 }; i < count; ++i)
            success &= _readAndDecode (result[i], decoder);
        return success;
    }
};

// specialization for encoding optional types
template<typename ValueT>
struct _ValueEncoder<std::optional<ValueT>>
{
    static inline void encodeAndAppend (MessageEncoder* encoder, const std::optional<ValueT>& argValue)
    {
        _appendToMessage (encoder, static_cast<int32_t> ((argValue) ? 1 : 0));
        if (argValue)
            _ValueEncoder<std::remove_const_t<ValueT>>::encodeAndAppend (encoder, *argValue);
    }
};

// specialization for decoding optional types
template<typename ValueT>
struct _ValueDecoder<std::optional<ValueT>>
{
    static inline bool readAndDecode (std::optional<ValueT>& result, const MessageDecoder* decoder)
    {
        result = std::nullopt;
        int32_t hasValue;
        if (!_readAndDecode (hasValue, decoder))
            return false;

        if (hasValue != 0)
        {
            ValueT value;
            if (!_readAndDecode (value, decoder))
                return false;
            result = std::make_optional<ValueT> (std::move (value));
        }
        return true;
    }
};


// specializations for en/decoding each ARA struct

#define ARA_IPC_HAS_ADDENDUM_MEMBER(var, member)                                                \
        /* \todo ARA_IMPLEMENTS_FIELD decorates the type with the ARA:: namespace,     */       \
        /* this conflicts with decltype's result - this copied version drops the ARA:: */       \
        (var.structSize > offsetof (std::remove_reference_t<decltype (var)>, member))

#define ARA_IPC_BEGIN_ENCODE(StructT)                                                           \
template<> struct _ValueEncoder<StructT> : public _CompoundValueEncoderBase<StructT>            \
{                                               /* specialization for given struct */           \
    using StructType = StructT;                                                                 \
    static inline void encode (MessageEncoder* encoder, const StructType& value)                \
    {
#define ARA_IPC_BEGIN_ENCODE_SIZED(StructT)                                                     \
        ARA_IPC_BEGIN_ENCODE (StructT)                                                          \
        _encodeAndAppend (encoder, value.structSize);
#define ARA_IPC_ENCODE_MEMBER(member)                                                           \
        _encodeAndAppend (encoder, value.member);
#define ARA_IPC_ENCODE_EMBEDDED_BYTES(member)                                                   \
        const BytesEncoder tmp_##member { reinterpret_cast<const uint8_t*> (value.member), sizeof (value.member), true }; \
        _encodeAndAppend (encoder, tmp_##member);
#define ARA_IPC_ENCODE_EMBEDDED_ARRAY(member)                                                   \
        const ArrayArgument<const std::remove_extent_t<decltype (value.member)>> tmp_##member { value.member, std::extent_v<decltype (value.member)> }; \
        _encodeAndAppend (encoder, tmp_##member);
#define ARA_IPC_ENCODE_VARIABLE_ARRAY(member, count)                                            \
        const ArrayArgument<const std::remove_pointer_t<decltype (value.member)>> tmp_##member { value.member, (value.member) ? value.count : 0 }; \
        _encodeAndAppend (encoder, tmp_##member);
#define ARA_IPC_ENCODE_OPTIONAL_MEMBER(member)                                                  \
        _encodeAndAppend (encoder, (value.member) ? std::make_optional (value.member) : std::nullopt);
#define ARA_IPC_ENCODE_ADDENDUM_MEMBER(member)                                                  \
        if (ARA_IPC_HAS_ADDENDUM_MEMBER (value, member))                                        \
            ARA_IPC_ENCODE_MEMBER (member)
#define ARA_IPC_ENCODE_OPTIONAL_ADDENDUM_MEMBER(member)                                         \
        if (ARA_IPC_HAS_ADDENDUM_MEMBER (value, member))                                        \
            ARA_IPC_ENCODE_OPTIONAL_MEMBER (member)
#define ARA_IPC_ENCODE_OPTIONAL_STRUCT_PTR(member)                                              \
        _encodeAndAppend (encoder, (value.member) ? std::make_optional (*value.member) : std::nullopt);
#define ARA_IPC_ENCODE_OPTIONAL_ADDENDUM_STRUCT_PTR(member)                                     \
        if (ARA_IPC_HAS_ADDENDUM_MEMBER (value, member))                                        \
            ARA_IPC_ENCODE_OPTIONAL_STRUCT_PTR (member)
#define ARA_IPC_ENCODE_ADDENDUM_VARIABLE_ARRAY(member, count)                                   \
        if (ARA_IPC_HAS_ADDENDUM_MEMBER (value, member)) {                                      \
            ARA_IPC_ENCODE_VARIABLE_ARRAY (member, count)                                       \
        } else {                                                                                \
            const ArrayArgument<const std::remove_pointer_t<decltype (value.member)>> tmp_##member { value.member, 0 }; \
            _encodeAndAppend (encoder, tmp_##member);                                           \
        }
#define ARA_IPC_END_ENCODE                                                                      \
    }                                                                                           \
};


#define ARA_IPC_BEGIN_DECODE(StructT)                                                           \
template<> struct _ValueDecoder<StructT> : public _CompoundValueDecoderBase<StructT>            \
{                                               /* specialization for given struct */           \
    using StructType = StructT;                                                                 \
    static inline bool decode (StructType& result, const MessageDecoder* decoder)               \
    {                                                                                           \
        bool success { true };
#define ARA_IPC_BEGIN_DECODE_SIZED(StructT)                                                     \
        ARA_IPC_BEGIN_DECODE (StructT)                                                          \
        success &= _readAndDecode (result.structSize, decoder);                                 \
        ARA_INTERNAL_ASSERT (success && (result.structSize >= k##StructT##MinSize));            \
        /* this relies on the IPC code always supporting all data types in the API, because */  \
        /* otherwise we'd need to limit structSize, since data beyond that is uninitialized */  \
        /* (could be implemented by tracking the highest decoded member while decoding).    */
#define ARA_IPC_DECODE_MEMBER(member)                                                           \
        success &= _readAndDecode (result.member, decoder);                                     \
        ARA_INTERNAL_ASSERT (success);
#define ARA_IPC_DECODE_EMBEDDED_BYTES(member)                                                   \
        auto resultSize_##member { sizeof (result.member) };                                    \
        BytesDecoder tmp_##member { reinterpret_cast<uint8_t*> (result.member), resultSize_##member }; \
        success &= _readAndDecode (tmp_##member, decoder);                                      \
        success &= (resultSize_##member == sizeof (result.member));                             \
        ARA_INTERNAL_ASSERT (success);
#define ARA_IPC_DECODE_EMBEDDED_ARRAY(member)                                                   \
        ArrayArgument<std::remove_extent_t<decltype (result.member)>> tmp_##member { result.member, std::extent_v<decltype (result.member)> }; \
        success &= _readAndDecode (tmp_##member, decoder);                                      \
        ARA_INTERNAL_ASSERT (success);
#define ARA_IPC_DECODE_VARIABLE_ARRAY(member, count, updateCount)                               \
        /* \todo the outer struct contains a pointer to the inner struct, so we need some */    \
        /*       thread-safe place to store it - is there a better way to achieve this?   */    \
        thread_local std::vector<typename std::remove_const_t<std::remove_pointer_t<decltype (result.member)>>> tmp_##member; \
        if (_readAndDecode (tmp_##member, decoder)) {                                           \
            result.member = tmp_##member.data ();                                               \
            if (updateCount) { result.count = tmp_##member.size (); }                           \
        } else {                                                                                \
            result.member = nullptr;                                                            \
            if (updateCount) { result.count = 0; }                                              \
        }
#define ARA_IPC_DECODE_OPTIONAL_MEMBER(member)                                                  \
        std::optional<std::remove_reference_t<decltype (result.member)>> tmp_##member;          \
        result.member = nullptr;                                                                \
        success &= _readAndDecode (tmp_##member, decoder);                                      \
        ARA_INTERNAL_ASSERT (success);                                                          \
        if (success && tmp_##member)                                                            \
            result.member = std::move (*tmp_##member);
#define ARA_IPC_DECODE_ADDENDUM_MEMBER(member)                                                  \
        if (ARA_IPC_HAS_ADDENDUM_MEMBER (result, member))                                       \
            { ARA_IPC_DECODE_MEMBER (member) }
#define ARA_IPC_DECODE_OPTIONAL_ADDENDUM_MEMBER(member)                                         \
        if (ARA_IPC_HAS_ADDENDUM_MEMBER (result, member))                                       \
           { ARA_IPC_DECODE_OPTIONAL_MEMBER (member) }
#define ARA_IPC_DECODE_ADDENDUM_VARIABLE_ARRAY(member, count, updateCount)                      \
        if (ARA_IPC_HAS_ADDENDUM_MEMBER (result, member))                                       \
           { ARA_IPC_DECODE_VARIABLE_ARRAY (member, count, updateCount) }
#define ARA_IPC_DECODE_OPTIONAL_STRUCT_PTR(member)                                              \
        /* \todo the outer struct contains a pointer to the inner struct, so we need some */    \
        /*       thread-safe place to store it - is there a better way to achieve this?   */    \
        thread_local std::optional<std::remove_const_t<std::remove_pointer_t<decltype (result.member)>>> cache; \
        if ((_readAndDecode (cache, decoder)) && (cache))                                       \
            result.member = &cache.value ();                                                    \
        else                                                                                    \
            result.member = nullptr;
#define ARA_IPC_DECODE_OPTIONAL_ADDENDUM_STRUCT_PTR(member)                                     \
        if (ARA_IPC_HAS_ADDENDUM_MEMBER (result, member))                                       \
            { ARA_IPC_DECODE_OPTIONAL_STRUCT_PTR (member) }
#define ARA_IPC_END_DECODE                                                                      \
        return success;                                                                         \
    }                                                                                           \
};


ARA_IPC_BEGIN_ENCODE (ARAColor)
    ARA_IPC_ENCODE_MEMBER (r)
    ARA_IPC_ENCODE_MEMBER (g)
    ARA_IPC_ENCODE_MEMBER (b)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE (ARAColor)
    ARA_IPC_DECODE_MEMBER (r)
    ARA_IPC_DECODE_MEMBER (g)
    ARA_IPC_DECODE_MEMBER (b)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE_SIZED (ARADocumentProperties)
    ARA_IPC_ENCODE_OPTIONAL_MEMBER (name)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE_SIZED (ARADocumentProperties)
    ARA_IPC_DECODE_OPTIONAL_MEMBER (name)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE_SIZED (ARAMusicalContextProperties)
    ARA_IPC_ENCODE_OPTIONAL_ADDENDUM_MEMBER (name)
    ARA_IPC_ENCODE_ADDENDUM_MEMBER (orderIndex)
    ARA_IPC_ENCODE_OPTIONAL_ADDENDUM_STRUCT_PTR (color)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE_SIZED (ARAMusicalContextProperties)
    ARA_IPC_DECODE_OPTIONAL_ADDENDUM_MEMBER (name)
    ARA_IPC_DECODE_ADDENDUM_MEMBER (orderIndex)
    ARA_IPC_DECODE_OPTIONAL_ADDENDUM_STRUCT_PTR (color)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE_SIZED (ARARegionSequenceProperties)
    ARA_IPC_ENCODE_OPTIONAL_MEMBER (name)
    ARA_IPC_ENCODE_MEMBER (orderIndex)
    ARA_IPC_ENCODE_MEMBER (musicalContextRef)
    ARA_IPC_ENCODE_OPTIONAL_ADDENDUM_STRUCT_PTR (color)
    ARA_IPC_ENCODE_OPTIONAL_ADDENDUM_MEMBER (persistentID)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE_SIZED (ARARegionSequenceProperties)
    ARA_IPC_DECODE_OPTIONAL_MEMBER (name)
    ARA_IPC_DECODE_MEMBER (orderIndex)
    ARA_IPC_DECODE_MEMBER (musicalContextRef)
    ARA_IPC_DECODE_OPTIONAL_ADDENDUM_STRUCT_PTR (color)
    ARA_IPC_DECODE_OPTIONAL_ADDENDUM_MEMBER (persistentID)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE_SIZED (ARAAudioSourceProperties)
    ARA_IPC_ENCODE_OPTIONAL_MEMBER (name)
    ARA_IPC_ENCODE_MEMBER (persistentID)
    ARA_IPC_ENCODE_MEMBER (sampleCount)
    ARA_IPC_ENCODE_MEMBER (sampleRate)
    ARA_IPC_ENCODE_MEMBER (channelCount)
    ARA_IPC_ENCODE_MEMBER (merits64BitSamples)
    ARA_IPC_ENCODE_ADDENDUM_MEMBER (channelArrangementDataType)
    if (ARA_IPC_HAS_ADDENDUM_MEMBER (value, channelArrangement))
    {
        // \todo potential endianess conversion is missing here if needed - but this is not known here,
        //       it must be performed in the calling code.
        //       this means the caller would need to copy the incoming data for mutation -
        //       maybe it would be better to move receiverEndianessMatches () from the sending side to
        //       the receiving side? We need to review how such a change would affect audio readers etc.
        const auto size { ChannelFormat::getChannelArrangementDataSize (value.channelCount, value.channelArrangementDataType, value.channelArrangement) };
        if (size > 0)
        {
            const BytesEncoder tmp_channelArrangement { reinterpret_cast<const uint8_t*> (value.channelArrangement), size, true };
            _encodeAndAppend (encoder, tmp_channelArrangement);
        }
    }
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE_SIZED (ARAAudioSourceProperties)
    ARA_IPC_DECODE_OPTIONAL_MEMBER (name)
    ARA_IPC_DECODE_MEMBER (persistentID)
    ARA_IPC_DECODE_MEMBER (sampleCount)
    ARA_IPC_DECODE_MEMBER (sampleRate)
    ARA_IPC_DECODE_MEMBER (channelCount)
    ARA_IPC_DECODE_MEMBER (merits64BitSamples)
    ARA_IPC_DECODE_ADDENDUM_MEMBER (channelArrangementDataType)
    if (result.structSize > offsetof (ARAAudioSourceProperties, channelArrangementDataType))
    {
        if (result.channelArrangementDataType == kARAChannelArrangementUndefined)
        {
            result.channelArrangement = nullptr;
        }
        else
        {
            /* \todo the outer struct contains a pointer to the inner struct, so we need some */
            /*       thread-safe place to store it - is there a better way to achieve this?   */
            thread_local ARAByte cache[2016UL];   // some arbitrary space that can be conveniently allocated
            auto resultSize_channelArrangement { sizeof (cache) };
            BytesDecoder tmp_channelArrangement { cache, resultSize_channelArrangement };
            if (_readAndDecode (tmp_channelArrangement, decoder) &&
                (resultSize_channelArrangement < sizeof (cache)))
            {
                result.channelArrangement = &cache;
            }
            else
            {
                result.channelArrangementDataType = kARAChannelArrangementUndefined;
                result.channelArrangement = nullptr;
                success = false;
            }
        }
    }
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE_SIZED (ARAAudioModificationProperties)
    ARA_IPC_ENCODE_OPTIONAL_MEMBER (name)
    ARA_IPC_ENCODE_MEMBER (persistentID)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE_SIZED (ARAAudioModificationProperties)
    ARA_IPC_DECODE_OPTIONAL_MEMBER (name)
    ARA_IPC_DECODE_MEMBER (persistentID)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE_SIZED (ARAPlaybackRegionProperties)
    ARA_IPC_ENCODE_MEMBER (transformationFlags)
    ARA_IPC_ENCODE_MEMBER (startInModificationTime)
    ARA_IPC_ENCODE_MEMBER (durationInModificationTime)
    ARA_IPC_ENCODE_MEMBER (startInPlaybackTime)
    ARA_IPC_ENCODE_MEMBER (durationInPlaybackTime)
    ARA_IPC_ENCODE_MEMBER (musicalContextRef)
    ARA_IPC_ENCODE_ADDENDUM_MEMBER (regionSequenceRef)
    ARA_IPC_ENCODE_OPTIONAL_ADDENDUM_MEMBER (name)
    ARA_IPC_ENCODE_OPTIONAL_ADDENDUM_STRUCT_PTR (color)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE_SIZED (ARAPlaybackRegionProperties)
    ARA_IPC_DECODE_MEMBER (transformationFlags)
    ARA_IPC_DECODE_MEMBER (startInModificationTime)
    ARA_IPC_DECODE_MEMBER (durationInModificationTime)
    ARA_IPC_DECODE_MEMBER (startInPlaybackTime)
    ARA_IPC_DECODE_MEMBER (durationInPlaybackTime)
    ARA_IPC_DECODE_MEMBER (musicalContextRef)
    ARA_IPC_DECODE_ADDENDUM_MEMBER (regionSequenceRef)
    ARA_IPC_DECODE_OPTIONAL_ADDENDUM_MEMBER (name)
    ARA_IPC_DECODE_OPTIONAL_ADDENDUM_STRUCT_PTR (color)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE (ARAContentTimeRange)
    ARA_IPC_ENCODE_MEMBER (start)
    ARA_IPC_ENCODE_MEMBER (duration)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE (ARAContentTimeRange)
    ARA_IPC_DECODE_MEMBER (start)
    ARA_IPC_DECODE_MEMBER (duration)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE (ARAContentTempoEntry)
    ARA_IPC_ENCODE_MEMBER (timePosition)
    ARA_IPC_ENCODE_MEMBER (quarterPosition)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE (ARAContentTempoEntry)
    ARA_IPC_DECODE_MEMBER (timePosition)
    ARA_IPC_DECODE_MEMBER (quarterPosition)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE (ARAContentBarSignature)
    ARA_IPC_ENCODE_MEMBER (numerator)
    ARA_IPC_ENCODE_MEMBER (denominator)
    ARA_IPC_ENCODE_MEMBER (position)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE (ARAContentBarSignature)
    ARA_IPC_DECODE_MEMBER (numerator)
    ARA_IPC_DECODE_MEMBER (denominator)
    ARA_IPC_DECODE_MEMBER (position)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE (ARAContentNote)
    ARA_IPC_ENCODE_MEMBER (frequency)
    ARA_IPC_ENCODE_MEMBER (pitchNumber)
    ARA_IPC_ENCODE_MEMBER (volume)
    ARA_IPC_ENCODE_MEMBER (startPosition)
    ARA_IPC_ENCODE_MEMBER (attackDuration)
    ARA_IPC_ENCODE_MEMBER (noteDuration)
    ARA_IPC_ENCODE_MEMBER (signalDuration)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE (ARAContentNote)
    ARA_IPC_DECODE_MEMBER (frequency)
    ARA_IPC_DECODE_MEMBER (pitchNumber)
    ARA_IPC_DECODE_MEMBER (volume)
    ARA_IPC_DECODE_MEMBER (startPosition)
    ARA_IPC_DECODE_MEMBER (attackDuration)
    ARA_IPC_DECODE_MEMBER (noteDuration)
    ARA_IPC_DECODE_MEMBER (signalDuration)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE (ARAContentTuning)
    ARA_IPC_ENCODE_MEMBER (concertPitchFrequency)
    ARA_IPC_ENCODE_MEMBER (root)
    ARA_IPC_ENCODE_EMBEDDED_ARRAY (tunings)
    ARA_IPC_ENCODE_OPTIONAL_MEMBER (name)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE (ARAContentTuning)
    ARA_IPC_DECODE_MEMBER (concertPitchFrequency)
    ARA_IPC_DECODE_MEMBER (root)
    ARA_IPC_DECODE_EMBEDDED_ARRAY (tunings)
    ARA_IPC_DECODE_OPTIONAL_MEMBER (name)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE (ARAContentKeySignature)
    ARA_IPC_ENCODE_MEMBER (root)
    ARA_IPC_ENCODE_EMBEDDED_BYTES (intervals)
    ARA_IPC_ENCODE_OPTIONAL_MEMBER (name)
    ARA_IPC_ENCODE_MEMBER (position)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE (ARAContentKeySignature)
    ARA_IPC_DECODE_MEMBER (root)
    ARA_IPC_DECODE_EMBEDDED_BYTES (intervals)
    ARA_IPC_DECODE_OPTIONAL_MEMBER (name)
    ARA_IPC_DECODE_MEMBER (position)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE (ARAContentChord)
    ARA_IPC_ENCODE_MEMBER (root)
    ARA_IPC_ENCODE_MEMBER (bass)
    ARA_IPC_ENCODE_EMBEDDED_BYTES (intervals)
    ARA_IPC_ENCODE_OPTIONAL_MEMBER (name)
    ARA_IPC_ENCODE_MEMBER (position)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE (ARAContentChord)
    ARA_IPC_DECODE_MEMBER (root)
    ARA_IPC_DECODE_MEMBER (bass)
    ARA_IPC_DECODE_EMBEDDED_BYTES (intervals)
    ARA_IPC_DECODE_OPTIONAL_MEMBER (name)
    ARA_IPC_DECODE_MEMBER (position)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE (ARAContentLyricsEntry)
    ARA_IPC_ENCODE_MEMBER (lyrics)
    ARA_IPC_ENCODE_MEMBER (continuesPreviousWord)
    ARA_IPC_ENCODE_MEMBER (language)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (phonemes, phonemeCount)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (phonemeOffsets, phonemeCount)
    ARA_IPC_ENCODE_MEMBER (phonemesGrade)
    ARA_IPC_ENCODE_MEMBER (position)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE (ARAContentLyricsEntry)
    ARA_IPC_DECODE_MEMBER (lyrics)
    ARA_IPC_DECODE_MEMBER (continuesPreviousWord)
    ARA_IPC_DECODE_MEMBER (language)
    ARA_IPC_DECODE_VARIABLE_ARRAY (phonemes, phonemeCount, true)
    ARA_IPC_DECODE_VARIABLE_ARRAY (phonemeOffsets, phonemeCount, false)
    ARA_IPC_DECODE_MEMBER (phonemesGrade)
    ARA_IPC_DECODE_MEMBER (position)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE_SIZED (ARARestoreObjectsFilter)
    ARA_IPC_ENCODE_MEMBER (documentData)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (audioSourceArchiveIDs, audioSourceIDsCount)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (audioSourceCurrentIDs, audioSourceIDsCount)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (audioModificationArchiveIDs, audioModificationIDsCount)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (audioModificationCurrentIDs, audioModificationIDsCount)
    ARA_IPC_ENCODE_ADDENDUM_VARIABLE_ARRAY (regionSequenceArchiveIDs, regionSequenceIDsCount)
    ARA_IPC_ENCODE_ADDENDUM_VARIABLE_ARRAY (regionSequenceCurrentIDs, regionSequenceIDsCount)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE_SIZED (ARARestoreObjectsFilter)
    ARA_IPC_DECODE_MEMBER (documentData)
    ARA_IPC_DECODE_VARIABLE_ARRAY (audioSourceArchiveIDs, audioSourceIDsCount, true)
    ARA_IPC_DECODE_VARIABLE_ARRAY (audioSourceCurrentIDs, audioSourceIDsCount, false)
    ARA_IPC_DECODE_VARIABLE_ARRAY (audioModificationArchiveIDs, audioModificationIDsCount, true)
    ARA_IPC_DECODE_VARIABLE_ARRAY (audioModificationCurrentIDs, audioModificationIDsCount, false)
    ARA_IPC_DECODE_ADDENDUM_VARIABLE_ARRAY (regionSequenceArchiveIDs, regionSequenceIDsCount, true)
    ARA_IPC_DECODE_ADDENDUM_VARIABLE_ARRAY (regionSequenceCurrentIDs, regionSequenceIDsCount, false)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE_SIZED (ARAStoreObjectsFilter)
    ARA_IPC_ENCODE_MEMBER (documentData)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (audioSourceRefs, audioSourceRefsCount)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (audioModificationRefs, audioModificationRefsCount)
    ARA_IPC_ENCODE_ADDENDUM_VARIABLE_ARRAY (regionSequenceRefs, regionSequenceRefsCount)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE_SIZED (ARAStoreObjectsFilter)
    ARA_IPC_DECODE_MEMBER (documentData)
    ARA_IPC_DECODE_VARIABLE_ARRAY (audioSourceRefs, audioSourceRefsCount, true)
    ARA_IPC_DECODE_VARIABLE_ARRAY (audioModificationRefs, audioModificationRefsCount, true)
    ARA_IPC_DECODE_ADDENDUM_VARIABLE_ARRAY (regionSequenceRefs, regionSequenceRefsCount, true)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE_SIZED (ARAProcessingAlgorithmProperties)
    ARA_IPC_ENCODE_MEMBER (persistentID)
    ARA_IPC_ENCODE_MEMBER (name)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE_SIZED (ARAProcessingAlgorithmProperties)
    ARA_IPC_DECODE_MEMBER (persistentID)
    ARA_IPC_DECODE_MEMBER (name)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE_SIZED (ARAViewSelection)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (playbackRegionRefs, playbackRegionRefsCount)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (regionSequenceRefs, regionSequenceRefsCount)
    ARA_IPC_ENCODE_OPTIONAL_STRUCT_PTR (timeRange)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE_SIZED (ARAViewSelection)
    ARA_IPC_DECODE_VARIABLE_ARRAY (playbackRegionRefs, playbackRegionRefsCount, true)
    ARA_IPC_DECODE_VARIABLE_ARRAY (regionSequenceRefs, regionSequenceRefsCount, true)
    ARA_IPC_DECODE_OPTIONAL_STRUCT_PTR (timeRange)
ARA_IPC_END_DECODE

ARA_IPC_BEGIN_ENCODE_SIZED (ARAFactory)
    ARA_IPC_ENCODE_MEMBER (lowestSupportedApiGeneration)
    ARA_IPC_ENCODE_MEMBER (highestSupportedApiGeneration)
    ARA_IPC_ENCODE_MEMBER (factoryID)
    ARA_IPC_ENCODE_MEMBER (plugInName)
    ARA_IPC_ENCODE_MEMBER (manufacturerName)
    ARA_IPC_ENCODE_MEMBER (informationURL)
    ARA_IPC_ENCODE_MEMBER (version)
    ARA_IPC_ENCODE_MEMBER (documentArchiveID)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (compatibleDocumentArchiveIDs, compatibleDocumentArchiveIDsCount)
    ARA_IPC_ENCODE_VARIABLE_ARRAY (analyzeableContentTypes, analyzeableContentTypesCount)
    ARA_IPC_ENCODE_MEMBER (supportedPlaybackTransformationFlags)
    ARA_IPC_ENCODE_ADDENDUM_MEMBER (supportsStoringAudioFileChunks)
    ARA_IPC_ENCODE_ADDENDUM_MEMBER (supportsSampleBasedAudioSources)
    ARA_IPC_ENCODE_ADDENDUM_MEMBER (supportsContentOnlyAudioSources)
    ARA_IPC_ENCODE_ADDENDUM_MEMBER (requiresPresetAudioSources)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE_SIZED (ARAFactory)
    ARA_IPC_DECODE_MEMBER (lowestSupportedApiGeneration)
    ARA_IPC_DECODE_MEMBER (highestSupportedApiGeneration)
    ARA_IPC_DECODE_MEMBER (factoryID)
    result.initializeARAWithConfiguration = nullptr;
    result.uninitializeARA = nullptr;
    ARA_IPC_DECODE_MEMBER (plugInName)
    ARA_IPC_DECODE_MEMBER (manufacturerName)
    ARA_IPC_DECODE_MEMBER (informationURL)
    ARA_IPC_DECODE_MEMBER (version)
    result.createDocumentControllerWithDocument = nullptr;
    ARA_IPC_DECODE_MEMBER (documentArchiveID)
    ARA_IPC_DECODE_VARIABLE_ARRAY (compatibleDocumentArchiveIDs, compatibleDocumentArchiveIDsCount, true)
    ARA_IPC_DECODE_VARIABLE_ARRAY (analyzeableContentTypes, analyzeableContentTypesCount, true)
    ARA_IPC_DECODE_MEMBER (supportedPlaybackTransformationFlags)
    ARA_IPC_DECODE_ADDENDUM_MEMBER (supportsStoringAudioFileChunks)
    ARA_IPC_DECODE_ADDENDUM_MEMBER (supportsSampleBasedAudioSources)
    ARA_IPC_DECODE_ADDENDUM_MEMBER (supportsContentOnlyAudioSources)
    ARA_IPC_DECODE_ADDENDUM_MEMBER (requiresPresetAudioSources)
ARA_IPC_END_DECODE


// ARADocumentControllerInterface::storeAudioSourceToAudioFileChunk() must return the documentArchiveID and the
// openAutomatically flag in addition to the return value, we need a special struct to encode this through IPC.
struct StoreAudioSourceToAudioFileChunkReply
{
    ARABool result;
    ARAPersistentID documentArchiveID;
    ARABool openAutomatically;
};
ARA_IPC_BEGIN_ENCODE (StoreAudioSourceToAudioFileChunkReply)
    ARA_IPC_ENCODE_MEMBER (result)
    ARA_IPC_ENCODE_MEMBER (documentArchiveID)
    ARA_IPC_ENCODE_MEMBER (openAutomatically)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE (StoreAudioSourceToAudioFileChunkReply)
    ARA_IPC_DECODE_MEMBER (result)
    ARA_IPC_DECODE_MEMBER (documentArchiveID)
    ARA_IPC_DECODE_MEMBER (openAutomatically)
ARA_IPC_END_DECODE

// ARADocumentControllerInterface::getPlaybackRegionHeadAndTailTime() must return both head- and tailtime.
struct GetPlaybackRegionHeadAndTailTimeReply
{
    ARATimeDuration headTime;
    ARATimeDuration tailTime;
};
ARA_IPC_BEGIN_ENCODE (GetPlaybackRegionHeadAndTailTimeReply)
    ARA_IPC_ENCODE_MEMBER (headTime)
    ARA_IPC_ENCODE_MEMBER (tailTime)
ARA_IPC_END_ENCODE
ARA_IPC_BEGIN_DECODE (GetPlaybackRegionHeadAndTailTimeReply)
    ARA_IPC_DECODE_MEMBER (headTime)
    ARA_IPC_DECODE_MEMBER (tailTime)
ARA_IPC_END_DECODE


#undef ARA_IPC_HAS_ADDENDUM_MEMBER

#undef ARA_IPC_BEGIN_ENCODE
#undef ARA_IPC_ENCODE_MEMBER
#undef ARA_IPC_ENCODE_EMBEDDED_BYES
#undef ARA_IPC_ENCODE_EMBEDDED_ARRAY
#undef ARA_IPC_ENCODE_VARIABLE_ARRAY
#undef ARA_IPC_ENCODE_OPTIONAL_MEMBER
#undef ARA_IPC_ENCODE_ADDENDUM_MEMBER
#undef ARA_IPC_ENCODE_OPTIONAL_ADDENDUM_MEMBER
#undef ARA_IPC_ENCODE_OPTIONAL_STRUCT_PTR
#undef ARA_IPC_ENCODE_OPTIONAL_ADDENDUM_STRUCT_PTR
#undef ARA_IPC_END_ENCODE

#undef ARA_IPC_BEGIN_DECODE
#undef ARA_IPC_BEGIN_DECODE_SIZED
#undef ARA_IPC_DECODE_MEMBER
#undef ARA_IPC_DECODE_EMBEDDED_BYTES
#undef ARA_IPC_DECODE_EMBEDDED_ARRAY
#undef ARA_IPC_DECODE_VARIABLE_ARRAY
#undef ARA_IPC_DECODE_OPTIONAL_MEMBER
#undef ARA_IPC_DECODE_ADDENDUM_MEMBER
#undef ARA_IPC_DECODE_ADDENDUM_VARIABLE_ARRAY
#undef ARA_IPC_DECODE_OPTIONAL_ADDENDUM_MEMBER
#undef ARA_IPC_DECODE_OPTIONAL_STRUCT_PTR
#undef ARA_IPC_DECODE_OPTIONAL_ADDENDUM_STRUCT_PTR
#undef ARA_IPC_END_DECODE


// actual definitions of wrapper functions to implicitly deduce _ValueEn-/Decoder<> -
// they are forward-declared above, before _ValueEn-/Decoder<> are defined
template <typename ValueT>
inline void _encodeAndAppend (MessageEncoder* encoder, const ValueT& argValue)
{
    return _ValueEncoder<ValueT>::encodeAndAppend (encoder, argValue);
}
template <typename ValueT>
inline bool _readAndDecode (ValueT& result, const MessageDecoder* decoder)
{
    return _ValueDecoder<ValueT>::readAndDecode (result, decoder);
}


//------------------------------------------------------------------------------


// private helpers for decodeArguments() to deal with the variable arguments one at a time
inline void _decodeArgumentsHelper (const MessageDecoder* /*decoder*/)
{
}
template<typename ArgT, typename... MoreArgs>
inline void _decodeArgumentsHelper (const MessageDecoder* decoder, ArgT& argValue, MoreArgs &... moreArgs)
{
    _readAndDecode (argValue, decoder);
    _decodeArgumentsHelper (decoder, moreArgs...);
}

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

template<typename ArgT, std::enable_if_t<!_IsStructPointerArg<ArgT>, int> = 0>
inline void _encodeAndAppendHelper (MessageEncoder* encoder, const ArgT& argValue)
{
    _encodeAndAppend (encoder, argValue);
}
template<typename ArgT, std::enable_if_t<_IsStructPointerArg<ArgT>, int> = 0>
inline void _encodeAndAppendHelper (MessageEncoder* encoder, const ArgT& argValue)
{
    _encodeAndAppend (encoder, (argValue) ? std::make_optional (*argValue) : std::nullopt);
}
inline void _encodeAndAppendHelper (MessageEncoder* encoder, const std::nullptr_t& /*argValue*/)
{
    // all optional types encode the value-not-contained case the same, so we can just pick any type here
    _encodeAndAppend (encoder, std::optional<size_t> ());
}

// caller side: create a message with the specified arguments
template<typename... Args>
inline void encodeArguments (MessageEncoder* encoder, const Args &... args)
{
    ((_encodeAndAppendHelper (encoder, args)), ...);
}

// caller side: decode the received reply to a sent message
template<typename RetT, std::enable_if_t<!std::is_class_v<RetT> || !std::is_pod_v<RetT>, bool> = true>
inline bool decodeReply (RetT& result, const MessageDecoder* decoder)
{
    return _readAndDecode (result, decoder);
}
template<typename RetT, std::enable_if_t<std::is_class_v<RetT> && std::is_pod_v<RetT>, bool> = true>
inline bool decodeReply (RetT& result, const MessageDecoder* decoder)
{
    return _ValueDecoder<RetT>::decode (result, decoder);
}


// callee side: decode the arguments of a received message
template<typename... Args>
inline void decodeArguments (const MessageDecoder* decoder, Args &... args)
{
    ARA_INTERNAL_ASSERT (decoder != nullptr);
    _decodeArgumentsHelper (decoder, args...);
}

// callee side: encode the reply to a received message
template<typename ValueT, std::enable_if_t<!std::is_class_v<ValueT> || !std::is_pod_v<ValueT>, bool> = true>
inline void encodeReply (MessageEncoder* encoder, const ValueT& value)
{
    ARA_INTERNAL_ASSERT (encoder != nullptr);
    _encodeAndAppend (encoder, value);
}
template<typename ValueT, std::enable_if_t<std::is_class_v<ValueT> && std::is_pod_v<ValueT>, bool> = true>
inline void encodeReply (MessageEncoder* encoder, const ValueT& value)
{
    ARA_INTERNAL_ASSERT (encoder != nullptr);
    _ValueEncoder<ValueT>::encode (encoder, value);
}


//------------------------------------------------------------------------------
// support for content readers
//------------------------------------------------------------------------------

inline void encodeContentEvent (MessageEncoder* encoder, const ARAContentType type, const void* eventData)
{
    switch (type)
    {
        case kARAContentTypeNotes:          encodeReply (encoder, *static_cast<const ARAContentNote*> (eventData)); break;
        case kARAContentTypeTempoEntries:   encodeReply (encoder, *static_cast<const ARAContentTempoEntry*> (eventData)); break;
        case kARAContentTypeBarSignatures:  encodeReply (encoder, *static_cast<const ARAContentBarSignature*> (eventData)); break;
        case kARAContentTypeStaticTuning:   encodeReply (encoder, *static_cast<const ARAContentTuning*> (eventData)); break;
        case kARAContentTypeKeySignatures:  encodeReply (encoder, *static_cast<const ARAContentKeySignature*> (eventData)); break;
        case kARAContentTypeSheetChords:    encodeReply (encoder, *static_cast<const ARAContentChord*> (eventData)); break;
        case kARAContentTypeLyricEntries:   encodeReply (encoder, *static_cast<const ARAContentLyricsEntry*> (eventData)); break;
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

    const void* decode (const MessageDecoder* decoder)
    {
        (this->*_decoderFunction) (decoder);
        return &_eventStorage;
    }

private:
    using _DecoderFunction = void (ContentEventDecoder::*) (const MessageDecoder*);

    template<typename ContentT>
    void _decodeContentEvent (const MessageDecoder* decoder)
    {
        decodeReply (*reinterpret_cast<ContentT*> (&this->_eventStorage), decoder);
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
    using CustomDecodeFunction = std::function<void (const MessageDecoder* decoder)>;

    explicit RemoteCaller (Connection* connection) noexcept
    : _connection { connection }
    {}

    template<typename... Args>
    void remoteCall (const MethodID methodID, const Args &... args) const
    {
        auto encoder { _connection->createEncoder () };
        encodeArguments (encoder.get (), args...);
        _connection->sendMessage (methodID.getMessageID (), std::move (encoder));
    }

    template<typename RetT, typename... Args>
    void remoteCall (RetT& result, const MethodID methodID, const Args &... args) const
    {
        auto encoder { _connection->createEncoder () };
        encodeArguments (encoder.get (), args...);
        _connection->sendMessage (methodID.getMessageID (), std::move (encoder),
                                  [&result] (const MessageDecoder* decoder)
                                  {
                                      ARA_INTERNAL_ASSERT (decoder);
                                      decodeReply (result, decoder);
                                  });
    }

    template<typename... Args>
    void remoteCall (CustomDecodeFunction& decodeFunction, const MethodID methodID, const Args &... args) const
    {
        auto encoder { _connection->createEncoder () };
        encodeArguments (encoder.get (), args...);
        _connection->sendMessage (methodID.getMessageID (), std::move (encoder),
                                  [&decodeFunction] (const MessageDecoder* decoder)
                                  {
                                      ARA_INTERNAL_ASSERT (decoder);
                                      decodeFunction (decoder);
                                  });
    }

    bool receiverEndianessMatches () const { return _connection->receiverEndianessMatches (); }

    Connection* getConnection () const { return _connection; }

private:
    Connection* const _connection;
};


}   // namespace IPC
}   // namespace ARA

//! @} ARA_Library_IPC


#endif // ARA_ENABLE_IPC

#endif // ARAIPCEncoding_h
