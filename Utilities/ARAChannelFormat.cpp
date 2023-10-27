//------------------------------------------------------------------------------
//! \file       ARAChannelFormat.cpp
//!             utility class dealing with the Companion-API-dependent surround channel arrangements
//! \project    ARA SDK Library
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

#include "ARAChannelFormat.h"

#include "ARA_Library/Debug/ARADebug.h"

#include <cstring>

#if defined (__APPLE__)
    // \todo hotfix: JUCE #defines Point as juce::Point, which clashes with QuickDraw's Point in MacTypes.h
    #if defined (Point)
        #undef Point
    #endif

    #include <CoreAudio/CoreAudioTypes.h>
#endif

namespace ARA {

bool ChannelFormat::operator== (const ChannelFormat& other) const noexcept
{
    if (_channelCount != other._channelCount)
        return false;

    if (_channelArrangementDataType != other._channelArrangementDataType)
        return false;

    if (_channelArrangement == other._channelArrangement)
        return true;

    ARA_INTERNAL_ASSERT (getDataSize () == other.getDataSize ());   // data size only depends on _channelCount and _channelArrangementDataType,
                                                                    // which are both compared above
    return (0 == std::memcmp (_channelArrangement, other._channelArrangement, getDataSize ()));
}

ARASize ChannelFormat::getDataSize () const noexcept
{
    switch (_channelArrangementDataType)
    {
        case kARAChannelArrangementUndefined:
        {
            return 0;
        }
        case kARAChannelArrangementVST3SpeakerArrangement:
        {
            return sizeof (uint64_t);
        }
        case kARAChannelArrangementCoreAudioChannelLayout:
        {
#if defined (__APPLE__)
            const auto audioChannelLayout { static_cast<const AudioChannelLayout*> (_channelArrangement) };
            return offsetof (AudioChannelLayout, mChannelDescriptions) +
                   sizeof (AudioChannelDescription) * audioChannelLayout->mNumberChannelDescriptions;
#else
            ARA_INTERNAL_ASSERT (false && "Core Audio data types can only be used on Apple platforms");
            return 0;
#endif
        }
        case kARAChannelArrangementAAXStemFormat:
        {
            return sizeof (uint32_t);
        }
        default:
        {
            ARA_INTERNAL_ASSERT (false && "unknown channel arrangement data type");
            return 0;
        }
    }
}

ARAChannelCount ChannelFormat::_getImpliedChannelCount (const ARAChannelArrangementDataType channelArrangementDataType,
                                                        const void* const channelArrangement) noexcept
{
    switch (channelArrangementDataType)
    {
        case kARAChannelArrangementUndefined:
        {
            return 0;
        }
        case kARAChannelArrangementVST3SpeakerArrangement:
        {
            // copied from Steinberg::Vst::SpeakerArr::getChannelCount () to avoid dependency on VST3 SDK in this library
            auto speakerArrangement { *static_cast<const uint64_t*> (channelArrangement) };
            ARAChannelCount arrangementChannelCount { 0 };
            while (speakerArrangement)
            {
                if ((speakerArrangement & 1UL) != 0)
                    ++arrangementChannelCount;
                speakerArrangement >>= 1;
            }
            return arrangementChannelCount;
        }
        case kARAChannelArrangementCoreAudioChannelLayout:
        {
#if defined (__APPLE__)
            const auto audioChannelLayout { static_cast<const AudioChannelLayout*> (channelArrangement) };
            const auto layoutChannelCount { (audioChannelLayout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions) ?
                                                    audioChannelLayout->mNumberChannelDescriptions :
                                                    AudioChannelLayoutTag_GetNumberOfChannels (audioChannelLayout->mChannelLayoutTag) };
            return static_cast<ARAChannelCount> (layoutChannelCount);
#else
            return 0;
#endif
        }
        case kARAChannelArrangementAAXStemFormat:
        {
            const auto stemFormat { *static_cast<const uint32_t*> (channelArrangement) };
            // copied from AAX_STEM_FORMAT_CHANNEL_COUNT() to avoid dependency on AAX SDK in this library
            return static_cast<uint16_t> (stemFormat & 0xFFFF);
        }
        default:
        {
            ARA_INTERNAL_ASSERT (false && "unknown channel arrangement data type");
            return 0;
        }
    }
}

bool ChannelFormat::isValid () const noexcept
{
    if (_channelArrangementDataType == kARAChannelArrangementUndefined)
    {
        // for undefined arrangement, the data pointer must be NULL
        if (_channelArrangement != nullptr)
            return false;

        // for more than stereo, a valid arrangement must be provided
        if (_channelCount > 2)
            return false;
    }
    else
    {
        if (_channelArrangement == nullptr)
            return false;

        const auto impliedChannelCount { _getImpliedChannelCount (_channelArrangementDataType, _channelArrangement) };
        if ((impliedChannelCount != 0) &&
            (impliedChannelCount != _channelCount))
            return false;

        if (_channelArrangementDataType == kARAChannelArrangementCoreAudioChannelLayout)
        {
#if defined (__APPLE__)
            // kAudioChannelLayoutTag_UseChannelBitmap is not allowed for Audio Units
            const auto audioChannelLayout { static_cast<const AudioChannelLayout*> (_channelArrangement) };
            if (audioChannelLayout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap)
                return false;
            // kAudioChannelLayoutTag_UseChannelDescriptions requires a description for each channel
            if (audioChannelLayout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions)
                return audioChannelLayout->mNumberChannelDescriptions == static_cast<UInt32> (_channelCount);
            else
                return audioChannelLayout->mNumberChannelDescriptions == 0;
#else
            ARA_INTERNAL_ASSERT (false && "Core Audio data types can only be used on Apple platforms");
            return false;
#endif
        }
    }
    return true;
}

}   // namespace ARA
