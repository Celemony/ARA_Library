//------------------------------------------------------------------------------
//! \file       ARAChannelFormat.h
//!             utility class dealing with the Companion-API-dependent surround channel arrangements
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

#ifndef ARAChannelArrangement_h
#define ARAChannelArrangement_h

#include "ARA_API/ARAInterface.h"

namespace ARA {

//! @addtogroup ARA_Library_Utility_Channel_Arrangement
//! @{

//! Helper class to parse the opaque, Companion-API-dependent channel arrangement information.
class ChannelFormat
{
public:
    //! Construct with channel count (defaults to 0) and unspecified channel arrangement.
    explicit ChannelFormat (const ARAChannelCount channelCount = 0)
    : _channelCount { channelCount },
      _channelArrangementDataType { kARAChannelArrangementUndefined },
      _channelArrangement { nullptr }
    {}

    //! Construct with channel count and explicit channel arrangement.
    ChannelFormat (const ARAChannelCount channelCount,
                   const ARAChannelArrangementDataType channelArrangementDataType,
                   const void* const channelArrangement)
    : ChannelFormat { channelCount } { update (channelCount, channelArrangementDataType, channelArrangement); }

    ~ChannelFormat () { update (_channelCount, kARAChannelArrangementUndefined, nullptr); }

    //! Assign new state.
    void update (const ARAChannelCount channelCount,
                 const ARAChannelArrangementDataType channelArrangementDataType,
                 const void* const channelArrangement);

    //! Validation helper for use at API boundary.
    bool isValid () const noexcept;

    //! Equality tests.
    bool operator== (const ChannelFormat& other) const noexcept;
    bool operator!= (const ChannelFormat& other) const noexcept { return !(*this == other); }

    //! Returns the channel count.
    ARAChannelCount getChannelCount () const noexcept { return _channelCount; }

    //! Returns the channel arrangement data type.
    ARAChannelArrangementDataType getChannelArrangementDataType () const noexcept { return _channelArrangementDataType; }

    //! Returns the Companion-API-dependent channel arrangement data.
    const void* getChannelArrangement () const noexcept { return _channelArrangement; }

    //! Returns the size of the opaque channel arrangement data.
    static ARASize getChannelArrangementDataSize (const ARAChannelCount channelCount,
                                                  const ARAChannelArrangementDataType channelArrangementDataType,
                                                  const void* const channelArrangement) noexcept;

private:
    // Returns the channel count encoded in the channel arrangement data.
    // May return 0 if the arrangement data does not imply a certain channel count.
    static ARAChannelCount _getImpliedChannelCount (const ARAChannelArrangementDataType channelArrangementDataType,
                                                    const void* const channelArrangement) noexcept;

private:
    ARAChannelCount _channelCount;
    ARAChannelArrangementDataType _channelArrangementDataType;
    void* _channelArrangement;
    ARAByte _arrangementStorage[24];
};

//! @} ARA_Library_Utility_Channel_Arrangement

}   // namespace ARA

#endif // ARAChannelArrangement_h
