//------------------------------------------------------------------------------
//! \file       ARAIPCMessage.cpp
//!             implementation of message en-/decoding
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

#include "ARAIPCMessage.h"


#if ARA_ENABLE_IPC


#include "ARA_Library/Debug/ARADebug.h"

#include <map>
#include <set>
#include <string_view>


namespace ARA {
namespace IPC {


FixedSizeMessageEncoder::FixedSizeMessageEncoder (size_t bufferSize)
: _buffer { new uint8_t[bufferSize] },
  _endOfBuffer { _buffer + bufferSize },
  _writeLocation { _buffer },
  _ownsBuffer { true }
{}

FixedSizeMessageEncoder::FixedSizeMessageEncoder (uint8_t* buffer, size_t bufferSize)
: _buffer { buffer },
  _endOfBuffer { _buffer + bufferSize },
  _writeLocation { _buffer },
  _ownsBuffer { false }
{}

FixedSizeMessageEncoder::~FixedSizeMessageEncoder ()
{
    if (_ownsBuffer)
        delete[] _buffer;
}

void FixedSizeMessageEncoder::appendBytes (const uint8_t* bytes, size_t size, bool /*copy*/)
{
    const auto newLocation { _writeLocation + size };
    if (newLocation > _endOfBuffer)
    {
        ARA_INTERNAL_ASSERT (false);
        return;
    }
    std::memcpy (_writeLocation, bytes, size);
    _writeLocation = newLocation;
}

#if defined (__APPLE__)
__attribute__((cf_returns_retained)) CFDataRef
#else
std::vector<uint8_t>
#endif
                                               FixedSizeMessageEncoder::convertToEncodedData ()
{
    if (_writeLocation == _buffer)
        return {};

#if defined (__APPLE__)
    const auto result { CFDataCreate (kCFAllocatorDefault, _buffer, static_cast<CFIndex> (_writeLocation - _buffer)) };
    ARA_INTERNAL_ASSERT (result);
    return result;
#else
    std::basic_string_view<uint8_t> view { _buffer, static_cast<size_t> (_writeLocation - _buffer) };
    return { view.begin (), view.end () };
#endif
}

std::unique_ptr<FixedSizeMessageEncoder> FixedSizeMessageEncoder::create ()
{
    return std::make_unique<FixedSizeMessageEncoder> (81920u);
}


VectorMessageEncoder::VectorMessageEncoder (size_t capacity)
{
    if (capacity > 0)
        _data.reserve (capacity);
}

void VectorMessageEncoder::appendBytes (const uint8_t* bytes, size_t size, bool /*copy*/)
{
    std::basic_string_view<uint8_t> view { bytes, size };
    _data.insert (_data.end (), view.begin (), view.end ());
}

#if defined (__APPLE__)
__attribute__((cf_returns_retained)) CFDataRef VectorMessageEncoder::convertToEncodedData ()
{
    if (_data.empty ())
        return nullptr;

    const auto result { CFDataCreate (kCFAllocatorDefault, _data.data (), static_cast<CFIndex> (_data.size ())) };
    ARA_INTERNAL_ASSERT (result);
    return result;
}
#else
// \todo it would be better to return a std::vector<uint8_t>&& here, but that's not compatible
//       with some of the other overrides...
std::vector<uint8_t> VectorMessageEncoder::convertToEncodedData ()
{
    return std::move(_data);
}
#endif

std::unique_ptr<VectorMessageEncoder> VectorMessageEncoder::create ()
{
    return std::make_unique<VectorMessageEncoder> ();
}


#if defined (__APPLE__)
CFMessageEncoder::CFMessageEncoder ()
: _data { CFDataCreateMutable (kCFAllocatorDefault, 0) }
{}

CFMessageEncoder::~CFMessageEncoder ()
{
    ARA_INTERNAL_ASSERT (!_data);       // should have been reset via convertToEncodedData()
    if (_data)
        CFRelease (_data);
}

void CFMessageEncoder::appendBytes (const uint8_t* bytes, size_t size, bool /*copy*/)
{
    CFDataAppendBytes (_data, static_cast<const UInt8*> (bytes), static_cast<CFIndex> (size));
}

__attribute__((cf_returns_retained)) CFDataRef CFMessageEncoder::convertToEncodedData ()
{
    ARA_INTERNAL_ASSERT (_data);        // convertToEncodedData() must only be called once
    const auto result { static_cast<CFDataRef> (_data) };
    _data = nullptr;
    return result;
}

std::unique_ptr<CFMessageEncoder> CFMessageEncoder::create ()
{
    return std::make_unique<CFMessageEncoder> ();
}
#endif  // defined (__APPLE__)


MessageDecoder::MessageDecoder (const uint8_t* buffer, size_t bufferSize)
: _buffer { buffer },
  _endOfBuffer { _buffer + bufferSize },
  _readLocation { _buffer }
{}

const uint8_t* MessageDecoder::readBytes (size_t size) const
{
    auto result { _readLocation };
    _readLocation += size;
    if (_readLocation > _endOfBuffer)
    {
        ARA_INTERNAL_ASSERT (false && "bytes are cut off");
        return nullptr;
    }
    return result;
}

std::unique_ptr<MessageDecoder> MessageDecoder::createWithMessageData (const uint8_t* data, const size_t dataSize)
{
    return std::make_unique<MessageDecoder> (data, dataSize);
}


#if defined (__APPLE__)
CFMessageDecoder::CFMessageDecoder (CFDataRef data, bool retain)
: MessageDecoder { CFDataGetBytePtr (data), static_cast<size_t> (CFDataGetLength (data)) },
  _data { data }
{
    if (data && retain)
        CFRetain (data);
}

CFMessageDecoder::~CFMessageDecoder ()
{
    if (_data)
        CFRelease (_data);
}

std::unique_ptr<CFMessageDecoder> CFMessageDecoder::createWithMessageData (CFDataRef data, bool isTemporary)
{
    // \todo IPC implementations typically use message buffers that are only valid during the callback -
    //       accordingly, we need to copy the message when dispatching to another thread.
    //       We currently copy upon creation, but we could optimize this by adding a call to the
    //       MessageDecoder API to trigger the copy only on-demand, when dispatching.
    //       But since currently most messages are dispatched anyways, we've postponed that for now.
    if (isTemporary)
        data = CFDataCreateCopy (kCFAllocatorDefault, data);
    return std::make_unique<CFMessageDecoder> (data, false);
}
#endif // defined (__APPLE__)


}   // namespace IPC
}   // namespace ARA

#endif // ARA_ENABLE_IPC
