//------------------------------------------------------------------------------
//! \file       ARAIPCProxyHost.cpp
//!             implementation of host-side ARA IPC proxy host
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

#include "ARAIPCProxyHost.h"


#if ARA_ENABLE_IPC

#include "ARA_Library/Dispatch/ARAHostDispatch.h"
#include "ARA_Library/Dispatch/ARAPlugInDispatch.h"

#if ARA_VALIDATE_API_CALLS
    #include "ARA_Library/Debug/ARAContentValidator.h"
    #include "ARA_Library/Utilities/ARAStdVectorUtilities.h"
#endif

#include <cstring>
#include <string>
#include <vector>


#if ARA_SUPPORT_VERSION_1
    #error "The ARA IPC proxy host implementation does not support ARA 1."
#endif


namespace ARA {
namespace IPC {
namespace ProxyHost {

class AudioAccessController;
class ArchivingController;
class ContentAccessController;
class ModelUpdateController;
class PlaybackController;
class DocumentController;


/*******************************************************************************/

struct RemoteAudioSource
{
    ARAAudioSourceHostRef mainHostRef;
    ARAAudioSourceRef plugInRef;
    ARAChannelCount channelCount;
};
ARA_MAP_REF (RemoteAudioSource, ARAAudioSourceRef)
ARA_MAP_HOST_REF (RemoteAudioSource, ARAAudioSourceHostRef)

struct RemoteAudioReader
{
    RemoteAudioSource* audioSource;
    ARAAudioReaderHostRef mainHostRef;
    size_t sampleSize;
    void (*swapFunction) (void* buffer, ARASampleCount sampleCount);
};
ARA_MAP_HOST_REF (RemoteAudioReader, ARAAudioReaderHostRef)

struct RemoteContentReader
{
    ARAContentReaderRef plugInRef;
    ARAContentType contentType;
};
ARA_MAP_REF (RemoteContentReader, ARAContentReaderRef)

struct RemoteHostContentReader
{
    RemoteHostContentReader (ARAContentReaderHostRef hostRef, ARAContentType type)
    : remoteHostRef { hostRef }, decoder { type } {}

    ARAContentReaderHostRef remoteHostRef;
    ContentEventDecoder decoder;
};
ARA_MAP_HOST_REF (RemoteHostContentReader, ARAContentReaderHostRef)


/*******************************************************************************/
//! Implementation of AudioAccessControllerInterface that channels all calls through IPC
class AudioAccessController : public Host::AudioAccessControllerInterface, public RemoteCaller
{
public:
    AudioAccessController (Sender& sender, ARAAudioAccessControllerHostRef remoteHostRef) noexcept
    : RemoteCaller { sender }, _remoteHostRef { remoteHostRef } {}

    ARAAudioReaderHostRef createAudioReaderForSource (ARAAudioSourceHostRef audioSourceHostRef, bool use64BitSamples) noexcept override;
    bool readAudioSamples (ARAAudioReaderHostRef audioReaderHostRef, ARASamplePosition samplePosition, ARASampleCount samplesPerChannel, void* const buffers[]) noexcept override;
    void destroyAudioReader (ARAAudioReaderHostRef audioReaderHostRef) noexcept override;

private:
    ARAAudioAccessControllerHostRef _remoteHostRef;
};

/*******************************************************************************/

void _swap (float* ptr)
{
    auto asIntPtr { reinterpret_cast<uint32_t*> (ptr) };
#if defined (_MSC_VER)
    *asIntPtr = _byteswap_ulong (*asIntPtr);
#elif defined (__GNUC__)
    *asIntPtr = __builtin_bswap32 (*asIntPtr);
#else
    #error "not implemented for this compiler."
#endif
}
void _swap (double* ptr)
{
    auto asIntPtr { reinterpret_cast<uint64_t*> (ptr) };
#if defined (_MSC_VER)
    *asIntPtr = _byteswap_uint64 (*asIntPtr);
#elif defined (__GNUC__)
    *asIntPtr = __builtin_bswap64 (*asIntPtr);
#else
    #error "not implemented for this compiler."
#endif
}

template<typename FloatT>
void _swapBuffer (void* buffer, ARASampleCount sampleCount)
{
    for (auto i { 0 }; i < sampleCount; ++i)
        _swap (static_cast<FloatT*> (buffer) + i);
}

ARAAudioReaderHostRef AudioAccessController::createAudioReaderForSource (ARAAudioSourceHostRef audioSourceHostRef, bool use64BitSamples) noexcept
{
    auto remoteAudioReader { new RemoteAudioReader };
    remoteAudioReader->audioSource = fromHostRef (audioSourceHostRef);
    remoteAudioReader->sampleSize = (use64BitSamples) ? sizeof (double) : sizeof (float);
    if (receiverEndianessMatches ())
        remoteAudioReader->swapFunction = nullptr;
    else
        remoteAudioReader->swapFunction = (use64BitSamples) ? &_swapBuffer<double> : &_swapBuffer<float>;
    remoteCallWithReply (remoteAudioReader->mainHostRef, ARA_IPC_HOST_METHOD_ID (ARAAudioAccessControllerInterface, createAudioReaderForSource), _remoteHostRef, remoteAudioReader->audioSource->mainHostRef, use64BitSamples);
    return toHostRef (remoteAudioReader);
}

bool AudioAccessController::readAudioSamples (ARAAudioReaderHostRef audioReaderHostRef, ARASamplePosition samplePosition,
                                              ARASampleCount samplesPerChannel, void* const buffers[]) noexcept
{
    auto remoteAudioReader { fromHostRef (audioReaderHostRef) };
    const auto channelCount { static_cast<size_t> (remoteAudioReader->audioSource->channelCount) };

    // recursively limit message size to keep IPC responsive
    if (samplesPerChannel > 8192)
    {
        const auto samplesPerChannel1 { samplesPerChannel / 2 };
        const auto result1 { readAudioSamples (audioReaderHostRef, samplePosition, samplesPerChannel1, buffers) };

        const auto samplesPerChannel2 { samplesPerChannel - samplesPerChannel1 };
        std::vector<void*> buffers2;
        buffers2.reserve (channelCount);
        for (auto i { 0U }; i < channelCount; ++i)
            buffers2.emplace_back (static_cast<uint8_t*> (buffers[i]) + static_cast<size_t> (samplesPerChannel1) * remoteAudioReader->sampleSize);

        if (result1)
        {
            return readAudioSamples (audioReaderHostRef, samplePosition + samplesPerChannel1, samplesPerChannel2, buffers2.data ());
        }
        else
        {
            for (auto i { 0U }; i < channelCount; ++i)
                std::memset (buffers2[i], 0, static_cast<size_t> (samplesPerChannel2) * remoteAudioReader->sampleSize);
            return false;
        }
    }

    // custom decoding to deal with float data memory ownership
    bool success { false };
    RemoteCaller::CustomDecodeFunction customDecode { [&success, &remoteAudioReader, &samplesPerChannel, &channelCount, &buffers] (const MessageDecoder& decoder) -> void
        {
            const auto bufferSize { remoteAudioReader->sampleSize * static_cast<size_t> (samplesPerChannel) };
            std::vector<size_t> resultSizes;
            std::vector<BytesDecoder> decoders;
            resultSizes.reserve (channelCount);
            decoders.reserve (channelCount);
            for (auto i { 0U }; i < channelCount; ++i)
            {
                resultSizes.emplace_back (bufferSize);
                decoders.emplace_back (static_cast<uint8_t*> (buffers[i]), resultSizes[i]);
            }

            ArrayArgument<BytesDecoder> channelData { decoders.data (), decoders.size () };
            success = decodeReply (channelData, decoder);
            if (success)
                ARA_INTERNAL_ASSERT (channelData.count == channelCount);

            for (auto i { 0U }; i < channelCount; ++i)
            {
                if (success)
                {
                    ARA_INTERNAL_ASSERT (resultSizes[i] == bufferSize);
                    if (remoteAudioReader->swapFunction)
                        remoteAudioReader->swapFunction (buffers[i], samplesPerChannel);
                }
                else
                {
                    std::memset (buffers[i], 0, bufferSize);
                }
            }

        } };
    remoteCallWithReply (customDecode, ARA_IPC_HOST_METHOD_ID (ARAAudioAccessControllerInterface, readAudioSamples),
                        _remoteHostRef, remoteAudioReader->mainHostRef, samplePosition, samplesPerChannel);
    return success;
}

void AudioAccessController::destroyAudioReader (ARAAudioReaderHostRef audioReaderHostRef) noexcept
{
    auto remoteAudioReader { fromHostRef (audioReaderHostRef) };
    remoteCallWithoutReply (ARA_IPC_HOST_METHOD_ID (ARAAudioAccessControllerInterface, destroyAudioReader), _remoteHostRef, remoteAudioReader->mainHostRef);
    delete remoteAudioReader;
}


/*******************************************************************************/
//! Implementation of ArchivingControllerInterface that channels all calls through IPC
class ArchivingController : public Host::ArchivingControllerInterface, public RemoteCaller
{
public:
    ArchivingController (Sender& sender, ARAArchivingControllerHostRef remoteHostRef) noexcept
    : RemoteCaller { sender }, _remoteHostRef { remoteHostRef } {}

    ARASize getArchiveSize (ARAArchiveReaderHostRef archiveReaderHostRef) noexcept override;
    bool readBytesFromArchive (ARAArchiveReaderHostRef archiveReaderHostRef, ARASize position, ARASize length, ARAByte buffer[]) noexcept override;
    bool writeBytesToArchive (ARAArchiveWriterHostRef archiveWriterHostRef, ARASize position, ARASize length, const ARAByte buffer[]) noexcept override;
    void notifyDocumentArchivingProgress (float value) noexcept override;
    void notifyDocumentUnarchivingProgress (float value) noexcept override;
    ARAPersistentID getDocumentArchiveID (ARAArchiveReaderHostRef archiveReaderHostRef) noexcept override;

private:
    ARAArchivingControllerHostRef _remoteHostRef;
    std::string _archiveID;
};

/*******************************************************************************/

ARASize ArchivingController::getArchiveSize (ARAArchiveReaderHostRef archiveReaderHostRef) noexcept
{
    ARASize size;
    remoteCallWithReply (size, ARA_IPC_HOST_METHOD_ID (ARAArchivingControllerInterface, getArchiveSize), _remoteHostRef, archiveReaderHostRef);
    return size;
}

bool ArchivingController::readBytesFromArchive (ARAArchiveReaderHostRef archiveReaderHostRef, ARASize position, ARASize length, ARAByte buffer[]) noexcept
{
    // recursively limit message size to keep IPC responsive
    if (length > 131072)
    {
        const auto length1 { length / 2 };
        const auto result1 { readBytesFromArchive (archiveReaderHostRef, position, length1, buffer) };

        const auto length2 { length - length1 };
        buffer += length1;
        if (result1)
        {
            return readBytesFromArchive (archiveReaderHostRef, position + length1, length2, buffer);
        }
        else
        {
            std::memset (buffer, 0, length2);
            return false;
        }
    }

    auto resultLength { length };
    BytesDecoder writer { buffer, resultLength };
    remoteCallWithReply (writer, ARA_IPC_HOST_METHOD_ID (ARAArchivingControllerInterface, readBytesFromArchive),
                        _remoteHostRef, archiveReaderHostRef, position, length);
    if (resultLength == length)
    {
        return true;
    }
    else
    {
        std::memset (buffer, 0, length);
        return false;
    }
}

bool ArchivingController::writeBytesToArchive (ARAArchiveWriterHostRef archiveWriterHostRef, ARASize position, ARASize length, const ARAByte buffer[]) noexcept
{
    // recursively limit message size to keep IPC responsive
    if (length > 131072)
    {
        const auto length1 { length / 2 };
        const auto result1 { writeBytesToArchive (archiveWriterHostRef, position, length1, buffer) };

        const auto length2 { length - length1 };
        buffer += length1;
        if (result1)
        {
            return writeBytesToArchive (archiveWriterHostRef, position + length1, length2, buffer);
        }
        else
        {
            return false;
        }
    }

    ARABool success;
    remoteCallWithReply (success, ARA_IPC_HOST_METHOD_ID (ARAArchivingControllerInterface, writeBytesToArchive),
                        _remoteHostRef, archiveWriterHostRef, position, BytesEncoder { buffer, length, false });
    return (success != kARAFalse);
}

void ArchivingController::notifyDocumentArchivingProgress (float value) noexcept
{
    remoteCallWithoutReply (ARA_IPC_HOST_METHOD_ID (ARAArchivingControllerInterface, notifyDocumentArchivingProgress), _remoteHostRef, value);
}

void ArchivingController::notifyDocumentUnarchivingProgress (float value) noexcept
{
    remoteCallWithoutReply (ARA_IPC_HOST_METHOD_ID (ARAArchivingControllerInterface, notifyDocumentUnarchivingProgress), _remoteHostRef, value);
}

ARAPersistentID ArchivingController::getDocumentArchiveID (ARAArchiveReaderHostRef archiveReaderHostRef) noexcept
{
    RemoteCaller::CustomDecodeFunction customDecode { [this] (const MessageDecoder& decoder) -> void
        {
            ARAPersistentID persistentID;
            decodeReply (persistentID, decoder);
            _archiveID.assign (persistentID);
        } };
    remoteCallWithReply (customDecode, ARA_IPC_HOST_METHOD_ID (ARAArchivingControllerInterface, getDocumentArchiveID), _remoteHostRef, archiveReaderHostRef);
    return _archiveID.c_str();
}


/*******************************************************************************/
//! Implementation of ContentAccessControllerInterface that channels all calls through IPC
class ContentAccessController : public Host::ContentAccessControllerInterface, public RemoteCaller
{
public:
    ContentAccessController (Sender& sender, ARAContentAccessControllerHostRef remoteHostRef) noexcept
    : RemoteCaller { sender }, _remoteHostRef { remoteHostRef } {}

    bool isMusicalContextContentAvailable (ARAMusicalContextHostRef musicalContextHostRef, ARAContentType type) noexcept override;
    ARAContentGrade getMusicalContextContentGrade (ARAMusicalContextHostRef musicalContextHostRef, ARAContentType type) noexcept override;
    ARAContentReaderHostRef createMusicalContextContentReader (ARAMusicalContextHostRef musicalContextHostRef, ARAContentType type, const ARAContentTimeRange* range) noexcept override;
    bool isAudioSourceContentAvailable (ARAAudioSourceHostRef audioSourceHostRef, ARAContentType type) noexcept override;
    ARAContentGrade getAudioSourceContentGrade (ARAAudioSourceHostRef audioSourceHostRef, ARAContentType type) noexcept override;
    ARAContentReaderHostRef createAudioSourceContentReader (ARAAudioSourceHostRef audioSourceHostRef, ARAContentType type, const ARAContentTimeRange* range) noexcept override;
    ARAInt32 getContentReaderEventCount (ARAContentReaderHostRef contentReaderHostRef) noexcept override;
    const void* getContentReaderDataForEvent (ARAContentReaderHostRef contentReaderHostRef, ARAInt32 eventIndex) noexcept override;
    void destroyContentReader (ARAContentReaderHostRef contentReaderHostRef) noexcept override;

private:
    ARAContentAccessControllerHostRef _remoteHostRef;
};

/*******************************************************************************/

bool ContentAccessController::isMusicalContextContentAvailable (ARAMusicalContextHostRef musicalContextHostRef, ARAContentType type) noexcept
{
    ARABool result;
    remoteCallWithReply (result, ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, isMusicalContextContentAvailable),
                        _remoteHostRef, musicalContextHostRef, type);
    return (result != kARAFalse);
}

ARAContentGrade ContentAccessController::getMusicalContextContentGrade (ARAMusicalContextHostRef musicalContextHostRef, ARAContentType type) noexcept
{
    ARAContentGrade grade;
    remoteCallWithReply (grade, ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, getMusicalContextContentGrade),
                        _remoteHostRef, musicalContextHostRef, type);
    return grade;
}

ARAContentReaderHostRef ContentAccessController::createMusicalContextContentReader (ARAMusicalContextHostRef musicalContextHostRef, ARAContentType type, const ARAContentTimeRange* range) noexcept
{
    ARAContentReaderHostRef contentReaderHostRef;
    remoteCallWithReply (contentReaderHostRef, ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, createMusicalContextContentReader),
                        _remoteHostRef, musicalContextHostRef, type, range);
    auto contentReader { new RemoteHostContentReader (contentReaderHostRef, type) };
    return toHostRef (contentReader);
}

bool ContentAccessController::isAudioSourceContentAvailable (ARAAudioSourceHostRef audioSourceHostRef, ARAContentType type) noexcept
{
    ARABool result;
    remoteCallWithReply (result, ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, isAudioSourceContentAvailable),
                        _remoteHostRef, fromHostRef (audioSourceHostRef)->mainHostRef, type);
    return (result != kARAFalse);
}

ARAContentGrade ContentAccessController::getAudioSourceContentGrade (ARAAudioSourceHostRef audioSourceHostRef, ARAContentType type) noexcept
{
    ARAContentGrade grade;
    remoteCallWithReply (grade, ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, getAudioSourceContentGrade),
                        _remoteHostRef, fromHostRef (audioSourceHostRef)->mainHostRef, type);
    return grade;
}

ARAContentReaderHostRef ContentAccessController::createAudioSourceContentReader (ARAAudioSourceHostRef audioSourceHostRef, ARAContentType type, const ARAContentTimeRange* range) noexcept
{
    ARAContentReaderHostRef contentReaderHostRef;
    remoteCallWithReply (contentReaderHostRef, ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, createAudioSourceContentReader),
                        _remoteHostRef, fromHostRef (audioSourceHostRef)->mainHostRef, type, range);
    auto contentReader { new RemoteHostContentReader (contentReaderHostRef, type) };
    return toHostRef (contentReader);
}

ARAInt32 ContentAccessController::getContentReaderEventCount (ARAContentReaderHostRef contentReaderHostRef) noexcept
{
    const auto contentReader { fromHostRef (contentReaderHostRef) };
    ARAInt32 count;
    remoteCallWithReply (count, ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, getContentReaderEventCount),
                        _remoteHostRef, contentReader->remoteHostRef);
    return count;
}

const void* ContentAccessController::getContentReaderDataForEvent (ARAContentReaderHostRef contentReaderHostRef, ARAInt32 eventIndex) noexcept
{
    const auto contentReader { fromHostRef (contentReaderHostRef) };
    const void* result {};
    RemoteCaller::CustomDecodeFunction customDecode { [&result, &contentReader] (const MessageDecoder& decoder) -> void
        {
            result = contentReader->decoder.decode (decoder);
        } };
    remoteCallWithReply (customDecode, ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, getContentReaderDataForEvent),
                        _remoteHostRef, contentReader->remoteHostRef, eventIndex);
    return result;
}

void ContentAccessController::destroyContentReader (ARAContentReaderHostRef contentReaderHostRef) noexcept
{
    const auto contentReader { fromHostRef (contentReaderHostRef) };
    remoteCallWithoutReply (ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, destroyContentReader), _remoteHostRef, contentReader->remoteHostRef);
    delete contentReader;
}


/*******************************************************************************/
//! Implementation of ModelUpdateControllerInterface that channels all calls through IPC
class ModelUpdateController : public Host::ModelUpdateControllerInterface, public RemoteCaller
{
public:
    ModelUpdateController (Sender& sender, ARAModelUpdateControllerHostRef remoteHostRef) noexcept
    : RemoteCaller { sender }, _remoteHostRef { remoteHostRef } {}

    void notifyAudioSourceAnalysisProgress (ARAAudioSourceHostRef audioSourceHostRef, ARAAnalysisProgressState state, float value) noexcept override;
    void notifyAudioSourceContentChanged (ARAAudioSourceHostRef audioSourceHostRef, const ARAContentTimeRange* range, ContentUpdateScopes scopeFlags) noexcept override;
    void notifyAudioModificationContentChanged (ARAAudioModificationHostRef audioModificationHostRef, const ARAContentTimeRange* range, ContentUpdateScopes scopeFlags) noexcept override;
    void notifyPlaybackRegionContentChanged (ARAPlaybackRegionHostRef playbackRegionHostRef, const ARAContentTimeRange* range, ContentUpdateScopes scopeFlags) noexcept override;

private:
    ARAModelUpdateControllerHostRef _remoteHostRef;
};

/*******************************************************************************/

void ModelUpdateController::notifyAudioSourceAnalysisProgress (ARAAudioSourceHostRef audioSourceHostRef, ARAAnalysisProgressState state, float value) noexcept
{
    remoteCallWithoutReply (ARA_IPC_HOST_METHOD_ID (ARAModelUpdateControllerInterface, notifyAudioSourceAnalysisProgress), _remoteHostRef, fromHostRef (audioSourceHostRef)->mainHostRef, state, value);
}

void ModelUpdateController::notifyAudioSourceContentChanged (ARAAudioSourceHostRef audioSourceHostRef, const ARAContentTimeRange* range, ContentUpdateScopes scopeFlags) noexcept
{
    remoteCallWithoutReply (ARA_IPC_HOST_METHOD_ID (ARAModelUpdateControllerInterface, notifyAudioSourceContentChanged), _remoteHostRef, fromHostRef (audioSourceHostRef)->mainHostRef, range, scopeFlags);
}

void ModelUpdateController::notifyAudioModificationContentChanged (ARAAudioModificationHostRef audioModificationHostRef, const ARAContentTimeRange* range, ContentUpdateScopes scopeFlags) noexcept
{
    remoteCallWithoutReply (ARA_IPC_HOST_METHOD_ID (ARAModelUpdateControllerInterface, notifyAudioModificationContentChanged), _remoteHostRef, audioModificationHostRef, range, scopeFlags);
}

void ModelUpdateController::notifyPlaybackRegionContentChanged (ARAPlaybackRegionHostRef playbackRegionHostRef, const ARAContentTimeRange* range, ContentUpdateScopes scopeFlags) noexcept
{
    remoteCallWithoutReply (ARA_IPC_HOST_METHOD_ID (ARAModelUpdateControllerInterface, notifyPlaybackRegionContentChanged), _remoteHostRef, playbackRegionHostRef, range, scopeFlags);
}


/*******************************************************************************/
//! Implementation of PlaybackControllerInterface that channels all calls through IPC
class PlaybackController : public Host::PlaybackControllerInterface, public RemoteCaller
{
public:
    PlaybackController (Sender& sender, ARAPlaybackControllerHostRef remoteHostRef) noexcept
    : RemoteCaller { sender }, _remoteHostRef { remoteHostRef } {}

    void requestStartPlayback () noexcept override;
    void requestStopPlayback () noexcept override;
    void requestSetPlaybackPosition (ARATimePosition timePosition) noexcept override;
    void requestSetCycleRange (ARATimePosition startTime, ARATimeDuration duration) noexcept override;
    void requestEnableCycle (bool enable) noexcept override;

private:
    ARAPlaybackControllerHostRef _remoteHostRef;
};

/*******************************************************************************/

void PlaybackController::requestStartPlayback () noexcept
{
    remoteCallWithoutReply (ARA_IPC_HOST_METHOD_ID (ARAPlaybackControllerInterface, requestStartPlayback), _remoteHostRef);
}

void PlaybackController::requestStopPlayback () noexcept
{
    remoteCallWithoutReply (ARA_IPC_HOST_METHOD_ID (ARAPlaybackControllerInterface, requestStopPlayback), _remoteHostRef);
}

void PlaybackController::requestSetPlaybackPosition (ARATimePosition timePosition) noexcept
{
    remoteCallWithoutReply (ARA_IPC_HOST_METHOD_ID (ARAPlaybackControllerInterface, requestSetPlaybackPosition), _remoteHostRef, timePosition);
}

void PlaybackController::requestSetCycleRange (ARATimePosition startTime, ARATimeDuration duration) noexcept
{
    remoteCallWithoutReply (ARA_IPC_HOST_METHOD_ID (ARAPlaybackControllerInterface, requestSetCycleRange), _remoteHostRef, startTime, duration);
}

void PlaybackController::requestEnableCycle (bool enable) noexcept
{
    remoteCallWithoutReply (ARA_IPC_HOST_METHOD_ID (ARAPlaybackControllerInterface, requestEnableCycle), _remoteHostRef, (enable) ? kARATrue : kARAFalse);
}


/*******************************************************************************/
//! Extension of Host::DocumentController that also stores the host instance visible to the plug-in
class DocumentController : public Host::DocumentController
{
public:
    explicit DocumentController (const Host::DocumentControllerHostInstance* hostInstance, const ARADocumentControllerInstance* instance) noexcept
      : Host::DocumentController { instance },
        _hostInstance { hostInstance }
    {}

    const Host::DocumentControllerHostInstance* getHostInstance () { return _hostInstance; }

private:
    const Host::DocumentControllerHostInstance* _hostInstance;
};
ARA_MAP_REF (DocumentController, ARADocumentControllerRef)


/*******************************************************************************/
//! Wrapper class for a plug-in extension instance that can forward remote calls to its sub-interfaces
class PlugInExtension
{
public:
    explicit PlugInExtension (const ARAPlugInExtensionInstance* instance)
    : _playbackRenderer { instance },
      _editorRenderer { instance },
      _editorView { instance }
    {}

    // Getters for ARA specific plug-in role interfaces
    Host::PlaybackRenderer* getPlaybackRenderer () { return &_playbackRenderer; }
    Host::EditorRenderer* getEditorRenderer () { return &_editorRenderer; }
    Host::EditorView* getEditorView () { return &_editorView; }

private:
    Host::PlaybackRenderer _playbackRenderer;
    Host::EditorRenderer _editorRenderer;
    Host::EditorView _editorView;
};
ARA_MAP_REF (PlugInExtension, ARAPlugInExtensionRef, ARAPlaybackRendererRef, ARAEditorRendererRef, ARAEditorViewRef)


/*******************************************************************************/

std::vector<const ARAFactory*> _factories {};
Sender* _plugInCallbacksSender {};

void addFactory (const ARAFactory* factory)
{
    ARA_INTERNAL_ASSERT(factory->highestSupportedApiGeneration >= kARAAPIGeneration_2_0_Final);
    ARA_INTERNAL_ASSERT(!ARA::contains (_factories, factory));

    _factories.emplace_back (factory);
}

void setPlugInCallbacksSender (Sender* plugInCallbacksSender)
{
    _plugInCallbacksSender = plugInCallbacksSender;
}

ARADocumentControllerRef getDocumentControllerRefForRemoteRef (ARADocumentControllerRef remoteRef)
{
    return fromRef (remoteRef)->getRef ();
}

ARAPlugInExtensionRef createPlugInExtension (const ARAPlugInExtensionInstance* instance)
{
    return toRef (new PlugInExtension { instance });
}

void destroyPlugInExtension (ARAPlugInExtensionRef plugInExtensionRef)
{
    delete fromRef (plugInExtensionRef);
}

void hostCommandHandler (const MessageID messageID, const MessageDecoder& decoder, MessageEncoder* const replyEncoder)
{
//  ARA_LOG ("hostCommandHandler received message %s", decodePlugInMessageID (messageID));

    // ARAFactory
    if (messageID == kGetFactoriesCountMessageID)
    {
        return encodeReply (replyEncoder, _factories.size ());
    }
    else if (messageID == kGetFactoryMessageID)
    {
        ARASize index;
        decodeArguments (decoder, index);
        ARA_INTERNAL_ASSERT (index < _factories.size ());
        return encodeReply (replyEncoder, *_factories[index]);
    }
    else if (messageID == kCreateDocumentControllerMessageID)
    {
        ARAPersistentID factoryID;
        ARAAudioAccessControllerHostRef audioAccessControllerHostRef;
        ARAArchivingControllerHostRef archivingControllerHostRef;
        ARABool provideContentAccessController;
        ARAContentAccessControllerHostRef contentAccessControllerHostRef;
        ARABool provideModelUpdateController;
        ARAModelUpdateControllerHostRef modelUpdateControllerHostRef;
        ARABool providePlaybackController;
        ARAPlaybackControllerHostRef playbackControllerHostRef;
        ARADocumentProperties properties;
        decodeArguments (decoder, factoryID,
                                audioAccessControllerHostRef, archivingControllerHostRef,
                                provideContentAccessController, contentAccessControllerHostRef,
                                provideModelUpdateController, modelUpdateControllerHostRef,
                                providePlaybackController, playbackControllerHostRef,
                                properties);

        const ARAFactory* factory {};
        for (const auto& f : _factories)
        {
            if (0 == std::strcmp (f->factoryID, factoryID))
            {
                factory = f;
                break;
            }
        }
        ARA_INTERNAL_ASSERT (factory != nullptr);
        if (factory != nullptr)
        {
            const auto audioAccessController { new AudioAccessController { *_plugInCallbacksSender, audioAccessControllerHostRef } };
            const auto archivingController { new ArchivingController { *_plugInCallbacksSender, archivingControllerHostRef } };
            const auto contentAccessController { (provideContentAccessController != kARAFalse) ? new ContentAccessController { *_plugInCallbacksSender, contentAccessControllerHostRef } : nullptr };
            const auto modelUpdateController { (provideModelUpdateController != kARAFalse) ? new ModelUpdateController { *_plugInCallbacksSender, modelUpdateControllerHostRef } : nullptr };
            const auto playbackController { (providePlaybackController != kARAFalse) ? new PlaybackController { *_plugInCallbacksSender, playbackControllerHostRef } : nullptr };

            const auto hostInstance { new Host::DocumentControllerHostInstance { audioAccessController, archivingController,
                                                                                    contentAccessController, modelUpdateController, playbackController } };

            auto documentControllerInstance { factory->createDocumentControllerWithDocument (hostInstance, &properties) };
            ARA_VALIDATE_API_CONDITION (documentControllerInstance != nullptr);
            ARA_VALIDATE_API_INTERFACE (documentControllerInstance->documentControllerInterface, ARADocumentControllerInterface);
            auto documentController { new DocumentController (hostInstance, documentControllerInstance) };
            return encodeReply (replyEncoder, ARADocumentControllerRef { toRef (documentController) });
        }
    }

    //ARADocumentControllerInterface
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyDocumentController))
    {
        ARADocumentControllerRef controllerRef;
        decodeArguments (decoder, controllerRef);
        auto documentController { fromRef (controllerRef) };
        documentController->destroyDocumentController ();

        delete documentController->getHostInstance ()->getPlaybackController ();
        delete documentController->getHostInstance ()->getModelUpdateController ();
        delete documentController->getHostInstance ()->getContentAccessController ();
        delete documentController->getHostInstance ()->getArchivingController ();
        delete documentController->getHostInstance ()->getAudioAccessController ();
        delete documentController->getHostInstance ();
        delete documentController;
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getFactory))
    {
        ARA_INTERNAL_ASSERT (false && "should never be queried here but instead cached from companion API upon setup");

        ARADocumentControllerRef controllerRef;
        decodeArguments (decoder, controllerRef);

        return encodeReply (replyEncoder, *(fromRef (controllerRef)->getFactory ()));
    }

    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, beginEditing))
    {
        ARADocumentControllerRef controllerRef;
        decodeArguments (decoder, controllerRef);

        fromRef (controllerRef)->beginEditing ();
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, endEditing))
    {
        ARADocumentControllerRef controllerRef;
        decodeArguments (decoder, controllerRef);

        fromRef (controllerRef)->endEditing ();
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, notifyModelUpdates))
    {
        ARADocumentControllerRef controllerRef;
        decodeArguments (decoder, controllerRef);

        fromRef (controllerRef)->notifyModelUpdates ();
    }

    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, restoreObjectsFromArchive))
    {
        ARADocumentControllerRef controllerRef;
        ARAArchiveReaderHostRef archiveReaderHostRef;
        OptionalArgument<ARARestoreObjectsFilter> filter;
        decodeArguments (decoder, controllerRef, archiveReaderHostRef, filter);

        return encodeReply (replyEncoder, fromRef (controllerRef)->restoreObjectsFromArchive (archiveReaderHostRef, (filter.second) ? &filter.first : nullptr) ? kARATrue : kARAFalse);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, storeObjectsToArchive))
    {
        ARADocumentControllerRef controllerRef;
        ARAArchiveWriterHostRef archiveWriterHostRef;
        OptionalArgument<ARAStoreObjectsFilter> filter;
        decodeArguments (decoder, controllerRef, archiveWriterHostRef, filter);

        std::vector<ARAAudioSourceRef> audioSourceRefs;
        if (filter.second && (filter.first.audioSourceRefsCount > 0))
        {
            audioSourceRefs.reserve (filter.first.audioSourceRefsCount);
            for (auto i { 0U }; i < filter.first.audioSourceRefsCount; ++i)
                audioSourceRefs.emplace_back (fromRef (filter.first.audioSourceRefs[i])->plugInRef);

            filter.first.audioSourceRefs = audioSourceRefs.data ();
        }

        return encodeReply (replyEncoder, fromRef (controllerRef)->storeObjectsToArchive (archiveWriterHostRef, (filter.second) ? &filter.first : nullptr) ? kARATrue : kARAFalse);
    }

    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateDocumentProperties))
    {
        ARADocumentControllerRef controllerRef;
        ARADocumentProperties properties;
        decodeArguments (decoder, controllerRef, properties);

        fromRef (controllerRef)->updateDocumentProperties (&properties);
    }

    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createMusicalContext))
    {
        ARADocumentControllerRef controllerRef;
        ARAMusicalContextHostRef hostRef;
        ARAMusicalContextProperties properties;
        decodeArguments (decoder, controllerRef, hostRef, properties);

        return encodeReply (replyEncoder, fromRef (controllerRef)->createMusicalContext (hostRef, &properties));
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateMusicalContextProperties))
    {
        ARADocumentControllerRef controllerRef;
        ARAMusicalContextRef musicalContextRef;
        ARAMusicalContextProperties properties;
        decodeArguments (decoder, controllerRef, musicalContextRef, properties);

        fromRef (controllerRef)->updateMusicalContextProperties (musicalContextRef, &properties);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateMusicalContextContent))
    {
        ARADocumentControllerRef controllerRef;
        ARAMusicalContextRef musicalContextRef;
        OptionalArgument<ARAContentTimeRange> range;
        ARAContentUpdateFlags flags;
        decodeArguments (decoder, controllerRef, musicalContextRef, range, flags);

        fromRef (controllerRef)->updateMusicalContextContent (musicalContextRef, (range.second) ? &range.first : nullptr, flags);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyMusicalContext))
    {
        ARADocumentControllerRef controllerRef;
        ARAMusicalContextRef musicalContextRef;
        decodeArguments (decoder, controllerRef, musicalContextRef);

        fromRef (controllerRef)->destroyMusicalContext (musicalContextRef);
    }

    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createRegionSequence))
    {
        ARADocumentControllerRef controllerRef;
        ARARegionSequenceHostRef hostRef;
        ARARegionSequenceProperties properties;
        decodeArguments (decoder, controllerRef, hostRef, properties);

        return encodeReply (replyEncoder, fromRef (controllerRef)->createRegionSequence (hostRef, &properties));
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateRegionSequenceProperties))
    {
        ARADocumentControllerRef controllerRef;
        ARARegionSequenceRef regionSequenceRef;
        ARARegionSequenceProperties properties;
        decodeArguments (decoder, controllerRef, regionSequenceRef, properties);

        fromRef (controllerRef)->updateRegionSequenceProperties (regionSequenceRef, &properties);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyRegionSequence))
    {
        ARADocumentControllerRef controllerRef;
        ARARegionSequenceRef regionSequenceRef;
        decodeArguments (decoder, controllerRef, regionSequenceRef);

        fromRef (controllerRef)->destroyRegionSequence (regionSequenceRef);
    }

    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createAudioSource))
    {
        auto remoteAudioSource { new RemoteAudioSource };

        ARADocumentControllerRef controllerRef;
        ARAAudioSourceProperties properties;
        decodeArguments (decoder, controllerRef, remoteAudioSource->mainHostRef, properties);

        remoteAudioSource->channelCount = properties.channelCount;
        remoteAudioSource->plugInRef = fromRef (controllerRef)->createAudioSource (toHostRef (remoteAudioSource), &properties);

        return encodeReply (replyEncoder, ARAAudioSourceRef { toRef (remoteAudioSource) });
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateAudioSourceProperties))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioSourceRef audioSourceRef;
        ARAAudioSourceProperties properties;
        decodeArguments (decoder, controllerRef, audioSourceRef, properties);

        fromRef (controllerRef)->updateAudioSourceProperties (fromRef (audioSourceRef)->plugInRef, &properties);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateAudioSourceContent))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioSourceRef audioSourceRef;
        OptionalArgument<ARAContentTimeRange> range;
        ARAContentUpdateFlags flags;
        decodeArguments (decoder, controllerRef, audioSourceRef, range, flags);

        fromRef (controllerRef)->updateAudioSourceContent (fromRef (audioSourceRef)->plugInRef, (range.second) ? &range.first : nullptr, flags);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, enableAudioSourceSamplesAccess))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioSourceRef audioSourceRef;
        ARABool enable;
        decodeArguments (decoder, controllerRef, audioSourceRef, enable);

        fromRef (controllerRef)->enableAudioSourceSamplesAccess (fromRef (audioSourceRef)->plugInRef, (enable) ? kARATrue : kARAFalse);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, deactivateAudioSourceForUndoHistory))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioSourceRef audioSourceRef;
        ARABool deactivate;
        decodeArguments (decoder, controllerRef, audioSourceRef, deactivate);

        fromRef (controllerRef)->deactivateAudioSourceForUndoHistory (fromRef (audioSourceRef)->plugInRef, (deactivate) ? kARATrue : kARAFalse);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, storeAudioSourceToAudioFileChunk))
    {
        ARADocumentControllerRef controllerRef;
        ARAArchiveWriterHostRef archiveWriterHostRef;
        ARAAudioSourceRef audioSourceRef;
        decodeArguments (decoder, controllerRef, archiveWriterHostRef, audioSourceRef);

        StoreAudioSourceToAudioFileChunkReply reply;
        bool openAutomatically;
        reply.result = (fromRef (controllerRef)->storeAudioSourceToAudioFileChunk (archiveWriterHostRef, fromRef (audioSourceRef)->plugInRef,
                                                                    &reply.documentArchiveID, &openAutomatically)) ? kARATrue : kARAFalse;
        reply.openAutomatically = (openAutomatically) ? kARATrue : kARAFalse;
        return encodeReply (replyEncoder, reply);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, isAudioSourceContentAnalysisIncomplete))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioSourceRef audioSourceRef;
        ARAContentType contentType;
        decodeArguments (decoder, controllerRef, audioSourceRef, contentType);

        return encodeReply (replyEncoder, fromRef (controllerRef)->isAudioSourceContentAnalysisIncomplete (fromRef (audioSourceRef)->plugInRef, contentType));
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, requestAudioSourceContentAnalysis))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioSourceRef audioSourceRef;
        std::vector<ARAContentType> contentTypes;
        decodeArguments (decoder, controllerRef, audioSourceRef, contentTypes);

        fromRef (controllerRef)->requestAudioSourceContentAnalysis (fromRef (audioSourceRef)->plugInRef, contentTypes.size (), contentTypes.data ());
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, isAudioSourceContentAvailable))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioSourceRef audioSourceRef;
        ARAContentType contentType;
        decodeArguments (decoder, controllerRef, audioSourceRef, contentType);

        return encodeReply (replyEncoder, (fromRef (controllerRef)->isAudioSourceContentAvailable (fromRef (audioSourceRef)->plugInRef, contentType)) ? kARATrue : kARAFalse);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getAudioSourceContentGrade))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioSourceRef audioSourceRef;
        ARAContentType contentType;
        decodeArguments (decoder, controllerRef, audioSourceRef, contentType);

        return encodeReply (replyEncoder, fromRef (controllerRef)->getAudioSourceContentGrade (fromRef (audioSourceRef)->plugInRef, contentType));
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createAudioSourceContentReader))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioSourceRef audioSourceRef;
        ARAContentType contentType;
        OptionalArgument<ARAContentTimeRange> range;
        decodeArguments (decoder, controllerRef, audioSourceRef, contentType, range);

        auto remoteContentReader { new RemoteContentReader };
        remoteContentReader->plugInRef = fromRef (controllerRef)->createAudioSourceContentReader (fromRef (audioSourceRef)->plugInRef, contentType, (range.second) ? &range.first : nullptr);
        remoteContentReader->contentType = contentType;
        return encodeReply (replyEncoder, ARAContentReaderRef { toRef (remoteContentReader) });
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyAudioSource))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioSourceRef audioSourceRef;
        decodeArguments (decoder, controllerRef, audioSourceRef);

        auto remoteAudioSource { fromRef (audioSourceRef) };
        fromRef (controllerRef)->destroyAudioSource (remoteAudioSource->plugInRef);

        delete remoteAudioSource;
    }

    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createAudioModification))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioSourceRef audioSourceRef;
        ARAAudioModificationHostRef hostRef;
        ARAAudioModificationProperties properties;
        decodeArguments (decoder, controllerRef, audioSourceRef, hostRef, properties);

        return encodeReply (replyEncoder, fromRef (controllerRef)->createAudioModification (fromRef (audioSourceRef)->plugInRef, hostRef, &properties));
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, cloneAudioModification))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioModificationRef audioModificationRef;
        ARAAudioModificationHostRef hostRef;
        ARAAudioModificationProperties properties;
        decodeArguments (decoder, controllerRef, audioModificationRef, hostRef, properties);

        return encodeReply (replyEncoder, fromRef (controllerRef)->cloneAudioModification (audioModificationRef, hostRef, &properties));
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateAudioModificationProperties))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioModificationRef audioModificationRef;
        ARAAudioModificationProperties properties;
        decodeArguments (decoder, controllerRef, audioModificationRef, properties);

        fromRef (controllerRef)->updateAudioModificationProperties (audioModificationRef, &properties);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, deactivateAudioModificationForUndoHistory))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioModificationRef audioModificationRef;
        ARABool deactivate;
        decodeArguments (decoder, controllerRef, audioModificationRef, deactivate);

        fromRef (controllerRef)->deactivateAudioModificationForUndoHistory (audioModificationRef, (deactivate) ? kARATrue : kARAFalse);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, isAudioModificationContentAvailable))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioModificationRef audioModificationRef;
        ARAContentType contentType;
        decodeArguments (decoder, controllerRef, audioModificationRef, contentType);

        return encodeReply (replyEncoder, (fromRef (controllerRef)->isAudioModificationContentAvailable (audioModificationRef, contentType)) ? kARATrue : kARAFalse);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getAudioModificationContentGrade))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioModificationRef audioModificationRef;
        ARAContentType contentType;
        decodeArguments (decoder, controllerRef, audioModificationRef, contentType);

        return encodeReply (replyEncoder, fromRef (controllerRef)->getAudioModificationContentGrade (audioModificationRef, contentType));
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createAudioModificationContentReader))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioModificationRef audioModificationRef;
        ARAContentType contentType;
        OptionalArgument<ARAContentTimeRange> range;
        decodeArguments (decoder, controllerRef, audioModificationRef, contentType, range);

        auto remoteContentReader { new RemoteContentReader };
        remoteContentReader->plugInRef = fromRef (controllerRef)->createAudioModificationContentReader (audioModificationRef, contentType, (range.second) ? &range.first : nullptr);
        remoteContentReader->contentType = contentType;
        return encodeReply (replyEncoder, ARAContentReaderRef { toRef (remoteContentReader) });
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyAudioModification))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioModificationRef audioModificationRef;
        decodeArguments (decoder, controllerRef, audioModificationRef);

        fromRef (controllerRef)->destroyAudioModification (audioModificationRef);
    }

    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createPlaybackRegion))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioModificationRef audioModificationRef;
        ARAPlaybackRegionHostRef hostRef;
        ARAPlaybackRegionProperties properties;
        decodeArguments (decoder, controllerRef, audioModificationRef, hostRef, properties);

        return encodeReply (replyEncoder, fromRef (controllerRef)->createPlaybackRegion (audioModificationRef, hostRef, &properties));
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updatePlaybackRegionProperties))
    {
        ARADocumentControllerRef controllerRef;
        ARAPlaybackRegionRef playbackRegionRef;
        ARAPlaybackRegionProperties properties;
        decodeArguments (decoder, controllerRef, playbackRegionRef, properties);

        fromRef (controllerRef)->updatePlaybackRegionProperties (playbackRegionRef, &properties);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getPlaybackRegionHeadAndTailTime))
    {
        ARADocumentControllerRef controllerRef;
        ARAPlaybackRegionRef playbackRegionRef;
        ARABool wantsHeadTime;
        ARABool wantsTailTime;
        decodeArguments (decoder, controllerRef, playbackRegionRef, wantsHeadTime, wantsTailTime);

        GetPlaybackRegionHeadAndTailTimeReply reply { 0.0, 0.0 };
        fromRef (controllerRef)->getPlaybackRegionHeadAndTailTime (playbackRegionRef, (wantsHeadTime != kARAFalse) ? &reply.headTime : nullptr,
                                                                                        (wantsTailTime != kARAFalse) ? &reply.tailTime : nullptr);
        return encodeReply (replyEncoder, reply);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, isPlaybackRegionContentAvailable))
    {
        ARADocumentControllerRef controllerRef;
        ARAPlaybackRegionRef playbackRegionRef;
        ARAContentType contentType;
        decodeArguments (decoder, controllerRef, playbackRegionRef, contentType);

        return encodeReply (replyEncoder, (fromRef (controllerRef)->isPlaybackRegionContentAvailable (playbackRegionRef, contentType)) ? kARATrue : kARAFalse);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getPlaybackRegionContentGrade))
    {
        ARADocumentControllerRef controllerRef;
        ARAPlaybackRegionRef playbackRegionRef;
        ARAContentType contentType;
        decodeArguments (decoder, controllerRef, playbackRegionRef, contentType);

        return encodeReply (replyEncoder, fromRef (controllerRef)->getPlaybackRegionContentGrade (playbackRegionRef, contentType));
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createPlaybackRegionContentReader))
    {
        ARADocumentControllerRef controllerRef;
        ARAPlaybackRegionRef playbackRegionRef;
        ARAContentType contentType;
        OptionalArgument<ARAContentTimeRange> range;
        decodeArguments (decoder, controllerRef, playbackRegionRef, contentType, range);

        auto remoteContentReader { new RemoteContentReader };
        remoteContentReader->plugInRef = fromRef (controllerRef)->createPlaybackRegionContentReader (playbackRegionRef, contentType, (range.second) ? &range.first : nullptr);
        remoteContentReader->contentType = contentType;
        return encodeReply (replyEncoder, ARAContentReaderRef { toRef (remoteContentReader) });
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyPlaybackRegion))
    {
        ARADocumentControllerRef controllerRef;
        ARAPlaybackRegionRef playbackRegionRef;
        decodeArguments (decoder, controllerRef, playbackRegionRef);

        fromRef (controllerRef)->destroyPlaybackRegion (playbackRegionRef);
    }

    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getContentReaderEventCount))
    {
        ARADocumentControllerRef controllerRef;
        ARAContentReaderRef contentReaderRef;
        decodeArguments (decoder, controllerRef, contentReaderRef);

        return encodeReply (replyEncoder, fromRef (controllerRef)->getContentReaderEventCount (fromRef (contentReaderRef)->plugInRef));
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getContentReaderDataForEvent))
    {
        ARADocumentControllerRef controllerRef;
        ARAContentReaderRef contentReaderRef;
        ARAInt32 eventIndex;
        decodeArguments (decoder, controllerRef, contentReaderRef, eventIndex);

        auto remoteContentReader { fromRef (contentReaderRef) };
        const void* eventData { fromRef (controllerRef)->getContentReaderDataForEvent (remoteContentReader->plugInRef, eventIndex) };
        return encodeContentEvent (replyEncoder, remoteContentReader->contentType, eventData);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyContentReader))
    {
        ARADocumentControllerRef controllerRef;
        ARAContentReaderRef contentReaderRef;
        decodeArguments (decoder, controllerRef, contentReaderRef);

        auto remoteContentReader { fromRef (contentReaderRef) };
        fromRef (controllerRef)->destroyContentReader (remoteContentReader->plugInRef);

        delete remoteContentReader;
    }

    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getProcessingAlgorithmsCount))
    {
        ARADocumentControllerRef controllerRef;
        decodeArguments (decoder, controllerRef);

        return encodeReply (replyEncoder, fromRef (controllerRef)->getProcessingAlgorithmsCount ());
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getProcessingAlgorithmProperties))
    {
        ARADocumentControllerRef controllerRef;
        ARAInt32 algorithmIndex;
        decodeArguments (decoder, controllerRef, algorithmIndex);

        return encodeReply (replyEncoder, *(fromRef (controllerRef)->getProcessingAlgorithmProperties (algorithmIndex)));
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getProcessingAlgorithmForAudioSource))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioSourceRef audioSourceRef;
        decodeArguments (decoder, controllerRef, audioSourceRef);

        return encodeReply (replyEncoder, fromRef (controllerRef)->getProcessingAlgorithmForAudioSource (fromRef (audioSourceRef)->plugInRef));
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, requestProcessingAlgorithmForAudioSource))
    {
        ARADocumentControllerRef controllerRef;
        ARAAudioSourceRef audioSourceRef;
        ARAInt32 algorithmIndex;
        decodeArguments (decoder, controllerRef, audioSourceRef, algorithmIndex);

        fromRef (controllerRef)->requestProcessingAlgorithmForAudioSource (fromRef (audioSourceRef)->plugInRef, algorithmIndex);
    }

    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, isLicensedForCapabilities))
    {
        ARADocumentControllerRef controllerRef;
        ARABool runModalActivationDialogIfNeeded;
        std::vector<ARAContentType> types;
        ARAPlaybackTransformationFlags transformationFlags;
        decodeArguments (decoder, controllerRef, runModalActivationDialogIfNeeded, types, transformationFlags);

        return encodeReply (replyEncoder, (fromRef (controllerRef)->isLicensedForCapabilities ((runModalActivationDialogIfNeeded != kARAFalse),
                                                                    types.size(), types.data (), transformationFlags)) ? kARATrue : kARAFalse);
    }

    // ARAPlaybackRendererInterface
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARAPlaybackRendererInterface, addPlaybackRegion))
    {
        ARAPlaybackRendererRef playbackRendererRef;
        ARAPlaybackRegionRef playbackRegionRef;
        decodeArguments (decoder, playbackRendererRef, playbackRegionRef);

        fromRef (playbackRendererRef)->getPlaybackRenderer ()->addPlaybackRegion (playbackRegionRef);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARAPlaybackRendererInterface, removePlaybackRegion))
    {
        ARAPlaybackRendererRef playbackRendererRef;
        ARAPlaybackRegionRef playbackRegionRef;
        decodeArguments (decoder, playbackRendererRef, playbackRegionRef);

        fromRef (playbackRendererRef)->getPlaybackRenderer ()->removePlaybackRegion (playbackRegionRef);
    }

    // ARAEditorRendererInterface
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARAEditorRendererInterface, addPlaybackRegion))
    {
        ARAEditorRendererRef editorRendererRef;
        ARAPlaybackRegionRef playbackRegionRef;
        decodeArguments (decoder, editorRendererRef, playbackRegionRef);

        fromRef (editorRendererRef)->getEditorRenderer ()->addPlaybackRegion (playbackRegionRef);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARAEditorRendererInterface, removePlaybackRegion))
    {
        ARAEditorRendererRef editorRendererRef;
        ARAPlaybackRegionRef playbackRegionRef;
        decodeArguments (decoder, editorRendererRef, playbackRegionRef);

        fromRef (editorRendererRef)->getEditorRenderer ()->removePlaybackRegion (playbackRegionRef);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARAEditorRendererInterface, addRegionSequence))
    {
        ARAEditorRendererRef editorRendererRef;
        ARARegionSequenceRef regionSequenceRef;
        decodeArguments (decoder, editorRendererRef, regionSequenceRef);

        fromRef (editorRendererRef)->getEditorRenderer ()->addRegionSequence (regionSequenceRef);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARAEditorRendererInterface, removeRegionSequence))
    {
        ARAEditorRendererRef editorRendererRef;
        ARARegionSequenceRef regionSequenceRef;
        decodeArguments (decoder, editorRendererRef, regionSequenceRef);

        fromRef (editorRendererRef)->getEditorRenderer ()->removeRegionSequence (regionSequenceRef);
    }

    // ARAEditorViewInterface
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARAEditorViewInterface, notifySelection))
    {
        ARAEditorViewRef editorViewRef;
        ARAViewSelection selection;
        decodeArguments (decoder, editorViewRef, selection);

        fromRef (editorViewRef)->getEditorView ()->notifySelection (&selection);
    }
    else if (messageID == ARA_IPC_PLUGIN_METHOD_ID (ARAEditorViewInterface, notifyHideRegionSequences))
    {
        ARAEditorViewRef editorViewRef;
        std::vector<ARARegionSequenceRef> regionSequenceRefs;
        decodeArguments (decoder, editorViewRef, regionSequenceRefs);

        fromRef (editorViewRef)->getEditorView ()->notifyHideRegionSequences (regionSequenceRefs.size (), regionSequenceRefs.data ());
    }

    else
    {
        ARA_INTERNAL_ASSERT (false && "unhandled message ID");
    }

    // all calls that create a reply return early from their respective if ().
// it is valid to provide a dummy replyEncoder if no reply has been requested.
//    ARA_INTERNAL_ASSERT (replyEncoder == nullptr);
}

}   // namespace ProxyHost
}   // namespace IPC
}   // namespace ARA

#endif // ARA_ENABLE_IPC
