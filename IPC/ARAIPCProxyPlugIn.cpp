//------------------------------------------------------------------------------
//! \file       ARAIPCProxyPlugIn.cpp
//!             implementation of host-side ARA IPC proxy plug-in
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

#include "ARAIPCProxyPlugIn.h"


#if ARA_ENABLE_IPC

#include "ARA_Library/Dispatch/ARAPlugInDispatch.h"
#include "ARA_Library/Dispatch/ARAHostDispatch.h"

#if ARA_VALIDATE_API_CALLS
    #include "ARA_Library/Debug/ARAContentValidator.h"
#endif

#include <cstring>
#include <set>
#include <string>


#if ARA_SUPPORT_VERSION_1
    #error "The ARA IPC proxy plug-in implementation does not support ARA 1."
#endif


/*******************************************************************************/
// configuration switches for debug output
// each can be defined as a nonzero integer to enable the associated logging

// log each entry from the host into the document controller (except for notifyModelUpdates (), which is called too often)
#ifndef ARA_ENABLE_HOST_ENTRY_LOG
    #define ARA_ENABLE_HOST_ENTRY_LOG 0
#endif

// log the creation and destruction of plug-in objects
#ifndef ARA_ENABLE_OBJECT_LIFETIME_LOG
    #define ARA_ENABLE_OBJECT_LIFETIME_LOG 0
#endif

// conditional logging helper functions based on the above switches
#if ARA_ENABLE_HOST_ENTRY_LOG
    #define ARA_LOG_HOST_ENTRY(object) ARA_LOG ("Host calls into %s (%p)", __FUNCTION__, object);
#else
    #define ARA_LOG_HOST_ENTRY(object) ((void) 0)
#endif

#if ARA_ENABLE_OBJECT_LIFETIME_LOG
    #define ARA_LOG_MODELOBJECT_LIFETIME(message, object)  ARA_LOG ("Plug success: document controller %p %s %p", object->getDocumentController (), message, object)
#else
    #define ARA_LOG_MODELOBJECT_LIFETIME(message, object) ((void) 0)
#endif


/*******************************************************************************/

namespace ARA {
namespace IPC {
namespace ProxyPlugIn {

struct AudioSource;
struct ContentReader;
struct HostContentReader;
struct HostAudioReader;
class DocumentController;
class PlaybackRenderer;
class EditorRenderer;
class EditorView;
class PlugInExtension;


/*******************************************************************************/
// ObjectRef validation helper class - empty class unless ARA_VALIDATE_API_CALLS is enabled

template<typename SubclassT>
class InstanceValidator
{
#if ARA_VALIDATE_API_CALLS
protected:
    inline InstanceValidator () noexcept
    {
        auto result { _instances.insert (this) };
        ARA_INTERNAL_ASSERT (result.second);
    }

    inline ~InstanceValidator ()
    {
        auto it { _instances.find (this) };
        ARA_INTERNAL_ASSERT (it != _instances.end ());
        _instances.erase (it);
    }

public:
    static inline bool isValid (const InstanceValidator* instance)
    {
        return _instances.find (instance) != _instances.end ();
    }

private:
    static std::set<const InstanceValidator*> _instances;
#endif
};

template<typename SubclassT>
std::set<const InstanceValidator<SubclassT>*> InstanceValidator<SubclassT>::_instances;

template<typename SubclassT>
inline bool isValidInstance (const SubclassT* instance)
{
    return InstanceValidator<SubclassT>::isValid (instance);
}


/*******************************************************************************/

struct AudioSource
#if ARA_VALIDATE_API_CALLS
                  : public InstanceValidator<AudioSource>
#endif
{
#if ARA_VALIDATE_API_CALLS
    AudioSource (ARAAudioSourceHostRef hostRef_, ARAAudioSourceRef remoteRef_, ARAChannelCount channelCount_,
                 ARASampleCount sampleCount_, ARASampleRate sampleRate_)
    : hostRef { hostRef_ }, remoteRef { remoteRef_ }, channelCount { channelCount_ },
      sampleCount { sampleCount_ }, sampleRate { sampleRate_ }
    {}
#endif

    ARAAudioSourceHostRef hostRef;
    ARAAudioSourceRef remoteRef;
    ARAChannelCount channelCount;
#if ARA_VALIDATE_API_CALLS
    ARASampleCount sampleCount;
    ARASampleRate sampleRate;
#endif
};
ARA_MAP_REF (AudioSource, ARAAudioSourceRef)
ARA_MAP_HOST_REF (AudioSource, ARAAudioSourceHostRef)

struct ContentReader
#if ARA_VALIDATE_API_CALLS
                     : public InstanceValidator<ContentReader>
#endif
{
#if ARA_VALIDATE_API_CALLS
    ContentReader (ARAContentReaderRef remoteRef_, ARAContentType type_)
    : remoteRef { remoteRef_ }, decoder { type_ }
    {}
#endif

    ARAContentReaderRef remoteRef;
    ContentEventDecoder decoder;
};
ARA_MAP_REF (ContentReader, ARAContentReaderRef)

struct HostContentReader
{
    ARAContentReaderHostRef hostRef;
    ARAContentType contentType;
};
ARA_MAP_HOST_REF (HostContentReader, ARAContentReaderHostRef)

struct HostAudioReader
{
    AudioSource* audioSource;
    ARAAudioReaderHostRef hostRef;
    size_t sampleSize;
};
ARA_MAP_HOST_REF (HostAudioReader, ARAAudioReaderHostRef)


/*******************************************************************************/
// Implementation of DocumentControllerInterface that channels all calls through IPC

class DocumentController : public PlugIn::DocumentControllerInterface, protected RemoteCaller, public InstanceValidator<DocumentController>
{
public:
    DocumentController (Sender& sender, const ARAFactory* factory, const ARADocumentControllerHostInstance* instance, const ARADocumentProperties* properties) noexcept;

public:
    template <typename StructType>
    using PropertiesPtr = PlugIn::PropertiesPtr<StructType>;

    // Destruction
    void destroyDocumentController () noexcept override;

    // Factory
    const ARAFactory* getFactory () const noexcept override;

    // Update Management
    void beginEditing () noexcept override;
    void endEditing () noexcept override;
    void notifyModelUpdates () noexcept override;

    // Archiving
    bool restoreObjectsFromArchive (ARAArchiveReaderHostRef archiveReaderHostRef, const ARARestoreObjectsFilter* filter) noexcept override;
    bool storeObjectsToArchive (ARAArchiveWriterHostRef archiveWriterHostRef, const ARAStoreObjectsFilter* filter) noexcept override;
    bool storeAudioSourceToAudioFileChunk (ARAArchiveWriterHostRef archiveWriterHostRef, ARAAudioSourceRef audioSourceRef, ARAPersistentID* documentArchiveID, bool* openAutomatically) noexcept override;

    // Document Management
    void updateDocumentProperties (PropertiesPtr<ARADocumentProperties> properties) noexcept override;

    // Musical Context Management
    ARAMusicalContextRef createMusicalContext (ARAMusicalContextHostRef hostRef, PropertiesPtr<ARAMusicalContextProperties> properties) noexcept override;
    void updateMusicalContextProperties (ARAMusicalContextRef musicalContextRef, PropertiesPtr<ARAMusicalContextProperties> properties) noexcept override;
    void updateMusicalContextContent (ARAMusicalContextRef musicalContextRef, const ARAContentTimeRange* range, ContentUpdateScopes flags) noexcept override;
    void destroyMusicalContext (ARAMusicalContextRef musicalContextRef) noexcept override;

    // Region Sequence Management
    ARARegionSequenceRef createRegionSequence (ARARegionSequenceHostRef hostRef, PropertiesPtr<ARARegionSequenceProperties> properties) noexcept override;
    void updateRegionSequenceProperties (ARARegionSequenceRef regionSequence, PropertiesPtr<ARARegionSequenceProperties> properties) noexcept override;
    void destroyRegionSequence (ARARegionSequenceRef regionSequence) noexcept override;

    // Audio Source Management
    ARAAudioSourceRef createAudioSource (ARAAudioSourceHostRef hostRef, PropertiesPtr<ARAAudioSourceProperties> properties) noexcept override;
    void updateAudioSourceProperties (ARAAudioSourceRef audioSourceRef, PropertiesPtr<ARAAudioSourceProperties> properties) noexcept override;
    void updateAudioSourceContent (ARAAudioSourceRef audioSourceRef, const ARAContentTimeRange* range, ContentUpdateScopes flags) noexcept override;
    void enableAudioSourceSamplesAccess (ARAAudioSourceRef audioSourceRef, bool enable) noexcept override;
    void deactivateAudioSourceForUndoHistory (ARAAudioSourceRef audioSourceRef, bool deactivate) noexcept override;
    void destroyAudioSource (ARAAudioSourceRef audioSourceRef) noexcept override;

    // Audio Modification Management
    ARAAudioModificationRef createAudioModification (ARAAudioSourceRef audioSourceRef, ARAAudioModificationHostRef hostRef, PropertiesPtr<ARAAudioModificationProperties> properties) noexcept override;
    ARAAudioModificationRef cloneAudioModification (ARAAudioModificationRef audioModificationRef, ARAAudioModificationHostRef hostRef, PropertiesPtr<ARAAudioModificationProperties> properties) noexcept override;
    void updateAudioModificationProperties (ARAAudioModificationRef audioModificationRef, PropertiesPtr<ARAAudioModificationProperties> properties) noexcept override;
    void deactivateAudioModificationForUndoHistory (ARAAudioModificationRef audioModificationRef, bool deactivate) noexcept override;
    void destroyAudioModification (ARAAudioModificationRef audioModificationRef) noexcept override;

    // Playback Region Management
    ARAPlaybackRegionRef createPlaybackRegion (ARAAudioModificationRef audioModificationRef, ARAPlaybackRegionHostRef hostRef, PropertiesPtr<ARAPlaybackRegionProperties> properties) noexcept override;
    void updatePlaybackRegionProperties (ARAPlaybackRegionRef playbackRegionRef, PropertiesPtr<ARAPlaybackRegionProperties> properties) noexcept override;
    void getPlaybackRegionHeadAndTailTime (ARAPlaybackRegionRef playbackRegionRef, ARATimeDuration* headTime, ARATimeDuration* tailTime) noexcept override;
    void destroyPlaybackRegion (ARAPlaybackRegionRef playbackRegionRef) noexcept override;

    // Content Reader Management
    bool isAudioSourceContentAvailable (ARAAudioSourceRef audioSourceRef, ARAContentType type) noexcept override;
    ARAContentGrade getAudioSourceContentGrade (ARAAudioSourceRef audioSourceRef, ARAContentType type) noexcept override;
    ARAContentReaderRef createAudioSourceContentReader (ARAAudioSourceRef audioSourceRef, ARAContentType type, const ARAContentTimeRange* range) noexcept override;

    bool isAudioModificationContentAvailable (ARAAudioModificationRef audioModificationRef, ARAContentType type) noexcept override;
    ARAContentGrade getAudioModificationContentGrade (ARAAudioModificationRef audioModificationRef, ARAContentType type) noexcept override;
    ARAContentReaderRef createAudioModificationContentReader (ARAAudioModificationRef audioModificationRef, ARAContentType type, const ARAContentTimeRange* range) noexcept override;

    bool isPlaybackRegionContentAvailable (ARAPlaybackRegionRef playbackRegionRef, ARAContentType type) noexcept override;
    ARAContentGrade getPlaybackRegionContentGrade (ARAPlaybackRegionRef playbackRegionRef, ARAContentType type) noexcept override;
    ARAContentReaderRef createPlaybackRegionContentReader (ARAPlaybackRegionRef playbackRegionRef, ARAContentType type, const ARAContentTimeRange* range) noexcept override;

    ARAInt32 getContentReaderEventCount (ARAContentReaderRef contentReaderRef) noexcept override;
    const void* getContentReaderDataForEvent (ARAContentReaderRef contentReaderRef, ARAInt32 eventIndex) noexcept override;
    void destroyContentReader (ARAContentReaderRef contentReaderRef) noexcept override;

    // Controlling Analysis
    bool isAudioSourceContentAnalysisIncomplete (ARAAudioSourceRef audioSourceRef, ARAContentType contentType) noexcept override;
    void requestAudioSourceContentAnalysis (ARAAudioSourceRef audioSourceRef, ARASize contentTypesCount, const ARAContentType contentTypes[]) noexcept override;

    ARAInt32 getProcessingAlgorithmsCount () noexcept override;
    const ARAProcessingAlgorithmProperties* getProcessingAlgorithmProperties (ARAInt32 algorithmIndex) noexcept override;
    ARAInt32 getProcessingAlgorithmForAudioSource (ARAAudioSourceRef audioSourceRef) noexcept override;
    void requestProcessingAlgorithmForAudioSource (ARAAudioSourceRef audioSourceRef, ARAInt32 algorithmIndex) noexcept override;

    // License Management
    bool isLicensedForCapabilities (bool runModalActivationDialogIfNeeded, ARASize contentTypesCount, const ARAContentType contentTypes[], ARAPlaybackTransformationFlags transformationFlags) noexcept override;

    // Accessors for Proxy
    const ARADocumentControllerInstance* getInstance () const noexcept { return &_instance; }
    ARADocumentControllerRef getRemoteRef () const noexcept { return _remoteRef; }

    // Host Interface Access
    PlugIn::HostAudioAccessController* getHostAudioAccessController () noexcept { return &_hostAudioAccessController; }
    PlugIn::HostArchivingController* getHostArchivingController () noexcept { return &_hostArchivingController; }
    PlugIn::HostContentAccessController* getHostContentAccessController () noexcept { return (_hostContentAccessController.isProvided ()) ? &_hostContentAccessController : nullptr; }
    PlugIn::HostModelUpdateController* getHostModelUpdateController () noexcept { return (_hostModelUpdateController.isProvided ()) ? &_hostModelUpdateController : nullptr; }
    PlugIn::HostPlaybackController* getHostPlaybackController () noexcept { return (_hostPlaybackController.isProvided ()) ? &_hostPlaybackController : nullptr; }

private:
    void destroyIfUnreferenced () noexcept;

    friend class PlugInExtension;
    void addPlugInExtension (PlugInExtension* plugInExtension) noexcept { _plugInExtensions.insert (plugInExtension); }
    void removePlugInExtension (PlugInExtension* plugInExtension) noexcept { _plugInExtensions.erase (plugInExtension); if (_plugInExtensions.empty ()) destroyIfUnreferenced (); }

private:
    const ARAFactory* const _factory;

    PlugIn::HostAudioAccessController _hostAudioAccessController;
    PlugIn::HostArchivingController _hostArchivingController;
    PlugIn::HostContentAccessController _hostContentAccessController;
    PlugIn::HostModelUpdateController _hostModelUpdateController;
    PlugIn::HostPlaybackController _hostPlaybackController;

    PlugIn::DocumentControllerInstance _instance;

    ARADocumentControllerRef _remoteRef;

    bool _hasBeenDestroyed { false };

    ARAProcessingAlgorithmProperties _processingAlgorithmData { 0, nullptr, nullptr };
    struct
    {
        std::string persistentID;
        std::string name;
    } _processingAlgorithmStrings;

    std::set<PlugInExtension*> _plugInExtensions;

    ARA_HOST_MANAGED_OBJECT (DocumentController)
};
ARA_MAP_HOST_REF (DocumentController, ARAAudioAccessControllerHostRef, ARAArchivingControllerHostRef,
                    ARAContentAccessControllerHostRef, ARAModelUpdateControllerHostRef, ARAPlaybackControllerHostRef)


/*******************************************************************************/

DocumentController::DocumentController (Sender& sender, const ARAFactory* factory, const ARADocumentControllerHostInstance* instance, const ARADocumentProperties* properties) noexcept
: RemoteCaller { sender },
  _factory { factory },
  _hostAudioAccessController { instance },
  _hostArchivingController { instance },
  _hostContentAccessController { instance },
  _hostModelUpdateController { instance },
  _hostPlaybackController { instance },
  _instance { this }
{
    ARAAudioAccessControllerHostRef audioAccessControllerHostRef { toHostRef (this) };
    ARAArchivingControllerHostRef archivingControllerHostRef { toHostRef (this) };
    ARAContentAccessControllerHostRef contentAccessControllerHostRef { toHostRef (this) };
    ARAModelUpdateControllerHostRef modelUpdateControllerHostRef { toHostRef (this) };
    ARAPlaybackControllerHostRef playbackControllerHostRef { toHostRef (this) };
    remoteCallWithReply (_remoteRef, kCreateDocumentControllerMessageID, _factory->factoryID,
                          audioAccessControllerHostRef, archivingControllerHostRef,
                          (_hostContentAccessController.isProvided ()) ? kARATrue : kARAFalse, contentAccessControllerHostRef,
                          (_hostModelUpdateController.isProvided ()) ? kARATrue : kARAFalse, modelUpdateControllerHostRef,
                          (_hostPlaybackController.isProvided ()) ? kARATrue : kARAFalse, playbackControllerHostRef,
                          properties);

    ARA_LOG_MODELOBJECT_LIFETIME ("did create document controller", _remoteRef);
}

void DocumentController::destroyDocumentController () noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    ARA_LOG_MODELOBJECT_LIFETIME ("will destroy document controller", _remoteRef);
    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyDocumentController), _remoteRef);

    _hasBeenDestroyed = true;

    destroyIfUnreferenced ();
}

void DocumentController::destroyIfUnreferenced () noexcept
{
    // still in use by host?
    if (!_hasBeenDestroyed)
        return;

    // still referenced from plug-in instances?
    if (!_plugInExtensions.empty ())
        return;

    delete this;
}

/*******************************************************************************/

const ARAFactory* DocumentController::getFactory () const noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    return _factory;
}

/*******************************************************************************/

void DocumentController::beginEditing () noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, beginEditing), _remoteRef);
}

void DocumentController::endEditing () noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, endEditing), _remoteRef);
}

void DocumentController::notifyModelUpdates () noexcept
{
#if ARA_ENABLE_HOST_ENTRY_LOG
    static int logCount { 0 };
    constexpr int maxLogCount { 3 };
    if ((++logCount) <= maxLogCount)
    {
        ARA_LOG_HOST_ENTRY (this);
        if (logCount >= maxLogCount)
            ARA_LOG ("notifyModelUpdates () called %i times, will now suppress logging future calls to it", maxLogCount);
    }
#endif
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    if (!_hostModelUpdateController.isProvided ())
        return;

    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, notifyModelUpdates), _remoteRef);
}

bool DocumentController::restoreObjectsFromArchive (ARAArchiveReaderHostRef archiveReaderHostRef, const ARARestoreObjectsFilter* filter) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    ARABool success;
    remoteCallWithReply (success, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, restoreObjectsFromArchive), _remoteRef, archiveReaderHostRef, filter);
    return (success != kARAFalse);
}

bool DocumentController::storeObjectsToArchive (ARAArchiveWriterHostRef archiveWriterHostRef, const ARAStoreObjectsFilter* filter) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    ARAStoreObjectsFilter tempFilter;
    std::vector<ARAAudioSourceRef> remoteAudioSourceRefs;
    if ((filter != nullptr) && (filter->audioSourceRefsCount > 0))
    {
        remoteAudioSourceRefs.reserve (filter->audioSourceRefsCount);
        for (auto i { 0U }; i < filter->audioSourceRefsCount; ++i)
            remoteAudioSourceRefs.emplace_back (fromRef (filter->audioSourceRefs[i])->remoteRef);

        tempFilter = *filter;
        tempFilter.audioSourceRefs = remoteAudioSourceRefs.data ();
        filter = &tempFilter;
    }

    ARABool success;
    remoteCallWithReply (success, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, storeObjectsToArchive), _remoteRef, archiveWriterHostRef, filter);
    return (success!= kARAFalse);
}

bool DocumentController::storeAudioSourceToAudioFileChunk (ARAArchiveWriterHostRef archiveWriterHostRef, ARAAudioSourceRef audioSourceRef, ARAPersistentID* documentArchiveID, bool* openAutomatically) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    auto audioSource { fromRef (audioSourceRef) };
    ARA_VALIDATE_API_ARGUMENT (audioSource, isValidInstance (audioSource));
    ARA_VALIDATE_API_ARGUMENT (documentArchiveID, documentArchiveID != nullptr);
    ARA_VALIDATE_API_ARGUMENT (openAutomatically, openAutomatically != nullptr);

    bool success { false };
    RemoteCaller::CustomDecodeFunction customDecode { [this, &success, &documentArchiveID, &openAutomatically] (const MessageDecoder& decoder) -> void
        {
            StoreAudioSourceToAudioFileChunkReply reply;
            decodeReply (reply, decoder);

            // find ID string in factory because our return value is a temporary copy
            if (0 == std::strcmp (reply.documentArchiveID, _factory->documentArchiveID))
            {
                *documentArchiveID = _factory->documentArchiveID;
            }
            else
            {
                *documentArchiveID = nullptr;
                for (auto i { 0U }; i < _factory->compatibleDocumentArchiveIDsCount; ++i)
                {
                    if (0 == std::strcmp (reply.documentArchiveID, _factory->compatibleDocumentArchiveIDs[i]))
                    {
                        *documentArchiveID = _factory->compatibleDocumentArchiveIDs[i];
                        break;
                    }
                }
                ARA_INTERNAL_ASSERT(*documentArchiveID != nullptr);
            }

            *openAutomatically = (reply.openAutomatically != kARAFalse);
            success = (reply.result != kARAFalse);
        } };
    remoteCallWithReply (customDecode, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, storeAudioSourceToAudioFileChunk),
                        _remoteRef, archiveWriterHostRef, audioSource->remoteRef);
    return success;
}

void DocumentController::updateDocumentProperties (PropertiesPtr<ARADocumentProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    ARA_VALIDATE_API_STRUCT_PTR (properties, ARADocumentProperties);

    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateDocumentProperties), _remoteRef, *properties);
}

/*******************************************************************************/

ARAMusicalContextRef DocumentController::createMusicalContext (ARAMusicalContextHostRef hostRef, PropertiesPtr<ARAMusicalContextProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    ARA_VALIDATE_API_STRUCT_PTR (properties, ARAMusicalContextProperties);

    ARAMusicalContextRef musicalContextRef;
    remoteCallWithReply (musicalContextRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createMusicalContext), _remoteRef, hostRef, *properties);

    ARA_LOG_MODELOBJECT_LIFETIME ("did create musical context", musicalContextRef);
    return musicalContextRef;
}

void DocumentController::updateMusicalContextProperties (ARAMusicalContextRef musicalContextRef, PropertiesPtr<ARAMusicalContextProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (musicalContextRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    ARA_VALIDATE_API_STRUCT_PTR (properties, ARAMusicalContextProperties);

    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateMusicalContextProperties), _remoteRef, musicalContextRef, *properties);
}

void DocumentController::updateMusicalContextContent (ARAMusicalContextRef musicalContextRef, const ARAContentTimeRange* range, ContentUpdateScopes flags) noexcept
{
    ARA_LOG_HOST_ENTRY (musicalContextRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateMusicalContextContent), _remoteRef, musicalContextRef, range, flags);
}

void DocumentController::destroyMusicalContext (ARAMusicalContextRef musicalContextRef) noexcept
{
    ARA_LOG_HOST_ENTRY (musicalContextRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    ARA_LOG_MODELOBJECT_LIFETIME ("will destroy musical context", musicalContextRef);
    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyMusicalContext), _remoteRef, musicalContextRef);
}

/*******************************************************************************/

ARARegionSequenceRef DocumentController::createRegionSequence (ARARegionSequenceHostRef hostRef, PropertiesPtr<ARARegionSequenceProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    ARA_VALIDATE_API_STRUCT_PTR (properties, ARARegionSequenceProperties);

    ARARegionSequenceRef regionSequenceRef;
    remoteCallWithReply (regionSequenceRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createRegionSequence), _remoteRef, hostRef, *properties);

    ARA_LOG_MODELOBJECT_LIFETIME ("did create region sequence", regionSequenceRef);
    return regionSequenceRef;
}

void DocumentController::updateRegionSequenceProperties (ARARegionSequenceRef regionSequenceRef, PropertiesPtr<ARARegionSequenceProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (regionSequenceRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    ARA_VALIDATE_API_STRUCT_PTR (properties, ARARegionSequenceProperties);

    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateRegionSequenceProperties), _remoteRef, regionSequenceRef, *properties);
}

void DocumentController::destroyRegionSequence (ARARegionSequenceRef regionSequenceRef) noexcept
{
    ARA_LOG_HOST_ENTRY (regionSequenceRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    ARA_LOG_MODELOBJECT_LIFETIME ("will destroy region sequence", regionSequenceRef);
    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyRegionSequence), _remoteRef, regionSequenceRef);
}

/*******************************************************************************/

ARAAudioSourceRef DocumentController::createAudioSource (ARAAudioSourceHostRef hostRef, PropertiesPtr<ARAAudioSourceProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    ARA_VALIDATE_API_STRUCT_PTR (properties, ARAAudioSourceProperties);

    auto audioSource { new AudioSource { hostRef, nullptr, properties->channelCount
#if ARA_VALIDATE_API_CALLS
                                                , properties->sampleCount, properties->sampleRate
#endif
                                        } };

    remoteCallWithReply (audioSource->remoteRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createAudioSource),
                        _remoteRef, ARAAudioSourceHostRef { toHostRef (audioSource) }, *properties);

    ARA_LOG_MODELOBJECT_LIFETIME ("did create audio source", audioSourceRef);
    return toRef (audioSource);
}

void DocumentController::updateAudioSourceProperties (ARAAudioSourceRef audioSourceRef, PropertiesPtr<ARAAudioSourceProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    auto audioSource { fromRef (audioSourceRef) };
    ARA_VALIDATE_API_ARGUMENT (audioSource, isValidInstance (audioSource));
    ARA_VALIDATE_API_STRUCT_PTR (properties, ARAAudioSourceProperties);

    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateAudioSourceProperties), _remoteRef, audioSource->remoteRef, *properties);
}

void DocumentController::updateAudioSourceContent (ARAAudioSourceRef audioSourceRef, const ARAContentTimeRange* range, ContentUpdateScopes flags) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    auto audioSource { fromRef (audioSourceRef) };
    ARA_VALIDATE_API_ARGUMENT (audioSource, isValidInstance (audioSource));

    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateAudioSourceContent), _remoteRef, audioSource->remoteRef, range, flags);
}

void DocumentController::enableAudioSourceSamplesAccess (ARAAudioSourceRef audioSourceRef, bool enable) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_VALIDATE_API_ARGUMENT (audioSource, isValidInstance (audioSource));

    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, enableAudioSourceSamplesAccess), _remoteRef, audioSource->remoteRef, (enable) ? kARATrue : kARAFalse);
}

void DocumentController::deactivateAudioSourceForUndoHistory (ARAAudioSourceRef audioSourceRef, bool deactivate) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_VALIDATE_API_ARGUMENT (audioSource, isValidInstance (audioSource));

    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, deactivateAudioSourceForUndoHistory), _remoteRef, audioSource->remoteRef, (deactivate) ? kARATrue : kARAFalse);
}

void DocumentController::destroyAudioSource (ARAAudioSourceRef audioSourceRef) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_VALIDATE_API_ARGUMENT (audioSource, isValidInstance (audioSource));

    ARA_LOG_MODELOBJECT_LIFETIME ("will destroy audio source", audioSource->remoteRef);
    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyAudioSource), _remoteRef, audioSource->remoteRef);
    delete audioSource;
}

/*******************************************************************************/

ARAAudioModificationRef DocumentController::createAudioModification (ARAAudioSourceRef audioSourceRef, ARAAudioModificationHostRef hostRef, PropertiesPtr<ARAAudioModificationProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    auto audioSource { fromRef (audioSourceRef) };
    ARA_VALIDATE_API_ARGUMENT (audioSource, isValidInstance (audioSource));
    ARA_VALIDATE_API_STRUCT_PTR (properties, ARAAudioModificationProperties);

    ARAAudioModificationRef audioModificationRef;
    remoteCallWithReply (audioModificationRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createAudioModification),
                        _remoteRef, audioSource->remoteRef, hostRef, *properties);

    ARA_LOG_MODELOBJECT_LIFETIME ("did create audio modification", audioModificationRef);
    return audioModificationRef;
}

ARAAudioModificationRef DocumentController::cloneAudioModification (ARAAudioModificationRef srcAudioModificationRef, ARAAudioModificationHostRef hostRef, PropertiesPtr<ARAAudioModificationProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (srcAudioModificationRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    ARA_VALIDATE_API_STRUCT_PTR (properties, ARAAudioModificationProperties);

    ARAAudioModificationRef clonedAudioModificationRef;
    remoteCallWithReply (clonedAudioModificationRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, cloneAudioModification),
                        _remoteRef, srcAudioModificationRef, hostRef, *properties);

    ARA_LOG_MODELOBJECT_LIFETIME ("did create cloned audio modification", clonedAudioModificationRef);
    return clonedAudioModificationRef;
}

void DocumentController::updateAudioModificationProperties (ARAAudioModificationRef audioModificationRef, PropertiesPtr<ARAAudioModificationProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (audioModificationRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    ARA_VALIDATE_API_STRUCT_PTR (properties, ARAAudioModificationProperties);

    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateAudioModificationProperties), _remoteRef, audioModificationRef, *properties);
}

void DocumentController::deactivateAudioModificationForUndoHistory (ARAAudioModificationRef audioModificationRef, bool deactivate) noexcept
{
    ARA_LOG_HOST_ENTRY (audioModificationRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, deactivateAudioModificationForUndoHistory), _remoteRef, audioModificationRef, (deactivate) ? kARATrue : kARAFalse);
}

void DocumentController::destroyAudioModification (ARAAudioModificationRef audioModificationRef) noexcept
{
    ARA_LOG_HOST_ENTRY (audioModificationRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    ARA_LOG_MODELOBJECT_LIFETIME ("will destroy audio modification", audioModification);
    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyAudioModification), _remoteRef, audioModificationRef);
}

/*******************************************************************************/

ARAPlaybackRegionRef DocumentController::createPlaybackRegion (ARAAudioModificationRef audioModificationRef, ARAPlaybackRegionHostRef hostRef, PropertiesPtr<ARAPlaybackRegionProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    ARA_VALIDATE_API_STRUCT_PTR (properties, ARAPlaybackRegionProperties);

    ARAPlaybackRegionRef playbackRegionRef;
    remoteCallWithReply (playbackRegionRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createPlaybackRegion),
                        _remoteRef, audioModificationRef, hostRef, *properties);

    ARA_LOG_MODELOBJECT_LIFETIME ("did create playback region", playbackRegionRef);
    return playbackRegionRef;
}

void DocumentController::updatePlaybackRegionProperties (ARAPlaybackRegionRef playbackRegionRef, PropertiesPtr<ARAPlaybackRegionProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (playbackRegionRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    ARA_VALIDATE_API_STRUCT_PTR (properties, ARAPlaybackRegionProperties);

    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updatePlaybackRegionProperties), _remoteRef, playbackRegionRef, *properties);
}

void DocumentController::getPlaybackRegionHeadAndTailTime (ARAPlaybackRegionRef playbackRegionRef, ARATimeDuration* headTime, ARATimeDuration* tailTime) noexcept
{
    ARA_LOG_HOST_ENTRY (playbackRegionRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    ARA_VALIDATE_API_ARGUMENT (headTime, headTime != nullptr);
    ARA_VALIDATE_API_ARGUMENT (tailTime, tailTime != nullptr);

    GetPlaybackRegionHeadAndTailTimeReply reply;
    remoteCallWithReply (reply, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getPlaybackRegionHeadAndTailTime),
                        _remoteRef, playbackRegionRef, (headTime != nullptr) ? kARATrue : kARAFalse, (tailTime != nullptr) ? kARATrue : kARAFalse);
    if (headTime != nullptr)
        *headTime = reply.headTime;
    if (tailTime != nullptr)
        *tailTime = reply.tailTime;
}

void DocumentController::destroyPlaybackRegion (ARAPlaybackRegionRef playbackRegionRef) noexcept
{
    ARA_LOG_HOST_ENTRY (playbackRegionRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    ARA_LOG_MODELOBJECT_LIFETIME ("will destroy playback region", playbackRegionRef);
    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyPlaybackRegion), _remoteRef, playbackRegionRef);
}

/*******************************************************************************/

bool DocumentController::isAudioSourceContentAvailable (ARAAudioSourceRef audioSourceRef, ARAContentType type) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));    const auto audioSource { fromRef (audioSourceRef) };
    ARA_VALIDATE_API_ARGUMENT (audioSource, isValidInstance (audioSource));

    ARABool result;
    remoteCallWithReply (result, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, isAudioSourceContentAvailable), _remoteRef, audioSource->remoteRef, type);
    return (result != kARAFalse);
}

ARAContentGrade DocumentController::getAudioSourceContentGrade (ARAAudioSourceRef audioSourceRef, ARAContentType type) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_VALIDATE_API_ARGUMENT (audioSource, isValidInstance (audioSource));

    ARAContentGrade grade;
    remoteCallWithReply (grade, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getAudioSourceContentGrade), _remoteRef, audioSource->remoteRef, type);
    return grade;
}

ARAContentReaderRef DocumentController::createAudioSourceContentReader (ARAAudioSourceRef audioSourceRef, ARAContentType type, const ARAContentTimeRange* range) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_VALIDATE_API_ARGUMENT (audioSource, isValidInstance (audioSource));

    ARAContentReaderRef contentReaderRef;
    remoteCallWithReply (contentReaderRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createAudioSourceContentReader),
                        _remoteRef, audioSource->remoteRef, type, range);

    auto contentReader { new ContentReader { contentReaderRef, type } };
#if ARA_ENABLE_OBJECT_LIFETIME_LOG
    ARA_LOG ("Plug success: did create content reader %p for audio source %p", contentReaderRef, audioSourceRef);
#endif
    return toRef (contentReader);
}

/*******************************************************************************/

bool DocumentController::isAudioModificationContentAvailable (ARAAudioModificationRef audioModificationRef, ARAContentType type) noexcept
{
    ARA_LOG_HOST_ENTRY (audioModificationRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    ARABool result;
    remoteCallWithReply (result, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, isAudioModificationContentAvailable), _remoteRef, audioModificationRef, type);
    return (result != kARAFalse);
}

ARAContentGrade DocumentController::getAudioModificationContentGrade (ARAAudioModificationRef audioModificationRef, ARAContentType type) noexcept
{
    ARA_LOG_HOST_ENTRY (audioModificationRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    ARAContentGrade grade;
    remoteCallWithReply (grade, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getAudioModificationContentGrade), _remoteRef, audioModificationRef, type);
    return grade;
}

ARAContentReaderRef DocumentController::createAudioModificationContentReader (ARAAudioModificationRef audioModificationRef, ARAContentType type, const ARAContentTimeRange* range) noexcept
{
    ARA_LOG_HOST_ENTRY (audioModificationRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    ARAContentReaderRef contentReaderRef;
    remoteCallWithReply (contentReaderRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createAudioModificationContentReader),
                        _remoteRef, audioModificationRef, type, range);

    auto contentReader { new ContentReader { contentReaderRef, type } };
#if ARA_ENABLE_OBJECT_LIFETIME_LOG
    ARA_LOG ("Plug success: did create content reader %p for audio modification %p", contentReaderRef, audioModificationRef);
#endif
    return toRef (contentReader);
}

/*******************************************************************************/

bool DocumentController::isPlaybackRegionContentAvailable (ARAPlaybackRegionRef playbackRegionRef, ARAContentType type) noexcept
{
    ARA_LOG_HOST_ENTRY (playbackRegionRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    ARABool result;
    remoteCallWithReply (result, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, isPlaybackRegionContentAvailable), _remoteRef, playbackRegionRef, type);
    return (result != kARAFalse);
}

ARAContentGrade DocumentController::getPlaybackRegionContentGrade (ARAPlaybackRegionRef playbackRegionRef, ARAContentType type) noexcept
{
    ARA_LOG_HOST_ENTRY (playbackRegionRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    ARAContentGrade grade;
    remoteCallWithReply (grade, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getPlaybackRegionContentGrade), _remoteRef, playbackRegionRef, type);
    return grade;
}

ARAContentReaderRef DocumentController::createPlaybackRegionContentReader (ARAPlaybackRegionRef playbackRegionRef, ARAContentType type, const ARAContentTimeRange* range) noexcept
{
    ARA_LOG_HOST_ENTRY (playbackRegionRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    ARAContentReaderRef contentReaderRef;
    remoteCallWithReply (contentReaderRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createPlaybackRegionContentReader),
                        _remoteRef, playbackRegionRef, type, range);

    auto contentReader { new ContentReader { contentReaderRef, type } };
#if ARA_ENABLE_OBJECT_LIFETIME_LOG
    ARA_LOG ("Plug success: did create content reader %p for playback region %p", contentReaderRef, playbackRegionRef);
#endif
    return toRef (contentReader);
}

/*******************************************************************************/

ARAInt32 DocumentController::getContentReaderEventCount (ARAContentReaderRef contentReaderRef) noexcept
{
    ARA_LOG_HOST_ENTRY (contentReaderRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    const auto contentReader { fromRef (contentReaderRef) };
    ARA_VALIDATE_API_ARGUMENT (contentReader, isValidInstance (contentReader));

    ARAInt32 count;
    remoteCallWithReply (count, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getContentReaderEventCount), _remoteRef, contentReader->remoteRef);
    return count;
}

const void* DocumentController::getContentReaderDataForEvent (ARAContentReaderRef contentReaderRef, ARAInt32 eventIndex) noexcept
{
    ARA_LOG_HOST_ENTRY (contentReaderRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    const auto contentReader { fromRef (contentReaderRef) };
    ARA_VALIDATE_API_ARGUMENT (contentReader, isValidInstance (contentReader));

    const void* result {};
    RemoteCaller::CustomDecodeFunction customDecode { [&result, &contentReader] (const MessageDecoder& decoder) -> void
        {
            result = contentReader->decoder.decode (decoder);
        } };
    remoteCallWithReply (customDecode, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getContentReaderDataForEvent),
                        _remoteRef, contentReader->remoteRef, eventIndex);
    return result;
}

void DocumentController::destroyContentReader (ARAContentReaderRef contentReaderRef) noexcept
{
    ARA_LOG_HOST_ENTRY (contentReaderRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    const auto contentReader { fromRef (contentReaderRef) };
    ARA_VALIDATE_API_ARGUMENT (contentReader, isValidInstance (contentReader));

    ARA_LOG_MODELOBJECT_LIFETIME ("will destroy content reader", contentReader->remoteRef);
    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyContentReader), _remoteRef, contentReader->remoteRef);

    delete contentReader;
}

/*******************************************************************************/

bool DocumentController::isAudioSourceContentAnalysisIncomplete (ARAAudioSourceRef audioSourceRef, ARAContentType type) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_VALIDATE_API_ARGUMENT (audioSource, isValidInstance (audioSource));

    ARABool result;
    remoteCallWithReply (result, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, isAudioSourceContentAnalysisIncomplete),
                        _remoteRef, audioSource->remoteRef, type);
    return (result != kARAFalse);
}

void DocumentController::requestAudioSourceContentAnalysis (ARAAudioSourceRef audioSourceRef, ARASize contentTypesCount, const ARAContentType contentTypes[]) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_VALIDATE_API_ARGUMENT (audioSource, isValidInstance (audioSource));

    const ArrayArgument<const ARAContentType> types { contentTypes, contentTypesCount };
    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, requestAudioSourceContentAnalysis), _remoteRef, audioSource->remoteRef, types);
}

ARAInt32 DocumentController::getProcessingAlgorithmsCount () noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    ARAInt32 count;
    remoteCallWithReply (count, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getProcessingAlgorithmsCount), _remoteRef);
    return count;
}

const ARAProcessingAlgorithmProperties* DocumentController::getProcessingAlgorithmProperties (ARAInt32 algorithmIndex) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    RemoteCaller::CustomDecodeFunction customDecode { [this] (const MessageDecoder& decoder) -> void
        {
            ARAProcessingAlgorithmProperties reply;
            decodeReply (reply, decoder);
            _processingAlgorithmStrings.persistentID = reply.persistentID;
            _processingAlgorithmStrings.name = reply.name;
            _processingAlgorithmData = reply;
            _processingAlgorithmData.persistentID = _processingAlgorithmStrings.persistentID.c_str ();
            _processingAlgorithmData.name = _processingAlgorithmStrings.name.c_str ();
        } };
    remoteCallWithReply (customDecode, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getProcessingAlgorithmProperties), _remoteRef, algorithmIndex);
    return &_processingAlgorithmData;
}

ARAInt32 DocumentController::getProcessingAlgorithmForAudioSource (ARAAudioSourceRef audioSourceRef) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_VALIDATE_API_ARGUMENT (audioSource, isValidInstance (audioSource));

    ARAInt32 result;
    remoteCallWithReply (result, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getProcessingAlgorithmForAudioSource), _remoteRef, audioSource->remoteRef);
    return result;
}

void DocumentController::requestProcessingAlgorithmForAudioSource (ARAAudioSourceRef audioSourceRef, ARAInt32 algorithmIndex) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_VALIDATE_API_ARGUMENT (audioSource, isValidInstance (audioSource));

    remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, requestProcessingAlgorithmForAudioSource), _remoteRef, audioSource->remoteRef, algorithmIndex);
}

/*******************************************************************************/

bool DocumentController::isLicensedForCapabilities (bool runModalActivationDialogIfNeeded, ARASize contentTypesCount, const ARAContentType contentTypes[], ARAPlaybackTransformationFlags transformationFlags) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

    const ArrayArgument<const ARAContentType> types { contentTypes, contentTypesCount };
    ARABool result;
    remoteCallWithReply (result, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, isLicensedForCapabilities),
                        _remoteRef, (runModalActivationDialogIfNeeded) ? kARATrue : kARAFalse, types, transformationFlags);
    return (result != kARAFalse);
}


/*******************************************************************************/
// Implementation of PlaybackRendererInterface that channels all calls through IPC

class PlaybackRenderer : public PlugIn::PlaybackRendererInterface, protected RemoteCaller, public InstanceValidator<PlaybackRenderer>
{
public:
    explicit PlaybackRenderer (Sender& sender, ARAPlaybackRendererRef remoteRef) noexcept
    : RemoteCaller { sender },
      _remoteRef { remoteRef }
    {}

    // Inherited public interface used by the C++ dispatcher, to be called by the ARAPlugInDispatch code exclusively.
    void addPlaybackRegion (ARAPlaybackRegionRef playbackRegionRef) noexcept override
    {
        ARA_LOG_HOST_ENTRY (this);
        ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

        remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARAPlaybackRendererInterface, addPlaybackRegion), _remoteRef, playbackRegionRef);
    }
    void removePlaybackRegion (ARAPlaybackRegionRef playbackRegionRef) noexcept override
    {
        ARA_LOG_HOST_ENTRY (this);
        ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

        remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARAPlaybackRendererInterface, removePlaybackRegion), _remoteRef, playbackRegionRef);
    }

private:
    ARAPlaybackRendererRef const _remoteRef;

    ARA_HOST_MANAGED_OBJECT (PlaybackRenderer)
};


/*******************************************************************************/
// Implementation of EditorRendererInterface that channels all calls through IPC

class EditorRenderer : public PlugIn::EditorRendererInterface, protected RemoteCaller, public InstanceValidator<EditorRenderer>
{
public:
    explicit EditorRenderer (Sender& sender, ARAEditorRendererRef remoteRef) noexcept
    : RemoteCaller { sender },
      _remoteRef { remoteRef }
    {}

    // Inherited public interface used by the C++ dispatcher, to be called by the ARAPlugInDispatch code exclusively.
    void addPlaybackRegion (ARAPlaybackRegionRef playbackRegionRef) noexcept override
    {
        ARA_LOG_HOST_ENTRY (this);
        ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

        remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARAEditorRendererInterface, addPlaybackRegion), _remoteRef, playbackRegionRef);
    }
    void removePlaybackRegion (ARAPlaybackRegionRef playbackRegionRef) noexcept override
    {
        ARA_LOG_HOST_ENTRY (this);
        ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

        remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARAEditorRendererInterface, removePlaybackRegion), _remoteRef, playbackRegionRef);
    }

    void addRegionSequence (ARARegionSequenceRef regionSequenceRef) noexcept override
    {
        ARA_LOG_HOST_ENTRY (this);
        ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

        remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARAEditorRendererInterface, addRegionSequence), _remoteRef, regionSequenceRef);
    }
    void removeRegionSequence (ARARegionSequenceRef regionSequenceRef) noexcept override
    {
        ARA_LOG_HOST_ENTRY (this);
        ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

        remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARAEditorRendererInterface, removeRegionSequence), _remoteRef, regionSequenceRef);
    }

private:
    ARAEditorRendererRef const _remoteRef;

    ARA_HOST_MANAGED_OBJECT (EditorRenderer)
};


/*******************************************************************************/
// Implementation of EditorRendererInterface that channels all calls through IPC

class EditorView : public PlugIn::EditorViewInterface, protected RemoteCaller, public InstanceValidator<EditorView>
{
public:
    explicit EditorView (Sender& sender, ARAEditorViewRef remoteRef) noexcept
    : RemoteCaller { sender },
      _remoteRef { remoteRef }
    {}

    // Inherited public interface used by the C++ dispatcher, to be called by the ARAPlugInDispatch code exclusively.
    void notifySelection (SizedStructPtr<ARAViewSelection> selection) noexcept override
    {
        ARA_LOG_HOST_ENTRY (this);
        ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));
        ARA_VALIDATE_API_STRUCT_PTR (selection, ARAViewSelection);

        remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARAEditorViewInterface, notifySelection), _remoteRef, *selection);
    }
    void notifyHideRegionSequences (ARASize regionSequenceRefsCount, const ARARegionSequenceRef regionSequenceRefs[]) noexcept override
    {
        ARA_LOG_HOST_ENTRY (this);
        ARA_VALIDATE_API_ARGUMENT (this, isValidInstance (this));

        const ArrayArgument<const ARARegionSequenceRef> sequences { regionSequenceRefs, regionSequenceRefsCount };
        remoteCallWithoutReply (ARA_IPC_PLUGIN_METHOD_ID (ARAEditorViewInterface, notifyHideRegionSequences), _remoteRef, sequences);
    }

private:
    ARAEditorViewRef const _remoteRef;

    ARA_HOST_MANAGED_OBJECT (EditorView)
};


/*******************************************************************************/
// implementation of ARAPlugInExtensionInstance that uses the above instance role classes

class PlugInExtension : public PlugIn::PlugInExtensionInstance
{
public:
    PlugInExtension (Sender& sender, ARADocumentControllerRef documentControllerRef,
                                      ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles,
                                      size_t remotePlugInExtensionRef) noexcept
    : PlugIn::PlugInExtensionInstance { (((knownRoles & kARAPlaybackRendererRole) == 0) || ((assignedRoles & kARAPlaybackRendererRole) != 0)) ?
                                            new PlaybackRenderer (sender, reinterpret_cast<ARAPlaybackRendererRef> (remotePlugInExtensionRef)) : nullptr,
                                        (((knownRoles & kARAEditorRendererRole) == 0) || ((assignedRoles & kARAEditorRendererRole) != 0)) ?
                                            new EditorRenderer (sender, reinterpret_cast<ARAEditorRendererRef> (remotePlugInExtensionRef)) : nullptr,
                                        (((knownRoles & kARAEditorViewRole) == 0) || ((assignedRoles & kARAEditorViewRole) != 0)) ?
                                            new EditorView (sender, reinterpret_cast<ARAEditorViewRef> (remotePlugInExtensionRef)) : nullptr },
      _documentController { PlugIn::fromRef<DocumentController> (documentControllerRef) }
    {
        plugInExtensionRef = reinterpret_cast<ARAPlugInExtensionRef> (remotePlugInExtensionRef);

        ARA_LOG_HOST_ENTRY (this);
        ARA_VALIDATE_API_ARGUMENT (documentControllerRef, isValidInstance (_documentController));

        _documentController->addPlugInExtension (this);

#if ARA_ENABLE_OBJECT_LIFETIME_LOG
        ARA_LOG ("Plug success: did create plug-in extension %p (playbackRenderer %p, editorRenderer %p, editorView %p)", this, getPlaybackRenderer (), getEditorRenderer (), getEditorView ());
#endif
    }

    ~PlugInExtension () noexcept
    {
        ARA_LOG_HOST_ENTRY (this);
#if ARA_ENABLE_OBJECT_LIFETIME_LOG
        ARA_LOG ("Plug success: will destroy plug-in extension %p (playbackRenderer %p, editorRenderer %p, editorView %p)", this, getPlaybackRenderer (), getEditorRenderer (), getEditorView ());
#endif

        _documentController->removePlugInExtension (this);

        delete getEditorView ();
        delete getEditorRenderer ();
        delete getPlaybackRenderer ();
    }

private:
    DocumentController* const _documentController;

    ARA_HOST_MANAGED_OBJECT (PlugInExtension)
};


/*******************************************************************************/
// Utility class that wraps an ARAFactory.

class Factory
{
public:
    Factory (Sender& hostCommandsSender, size_t index);

    const ARADocumentControllerInstance* createDocumentControllerWithDocument (const ARADocumentControllerHostInstance* hostInstance,
                                                                               const ARADocumentProperties* properties);

    const ARAFactory* getFactory () const { return &_factory; }
    Sender& getHostCommandsSender () const { return _hostCommandsSender; }

private:
    Sender& _hostCommandsSender;

    ARAFactory _factory;
    struct
    {
        std::string factoryID;
        std::string plugInName;
        std::string manufacturerName;
        std::string informationURL;
        std::string version;
        std::string documentArchiveID;
    } _factoryStrings;
    std::vector<std::string> _factoryCompatibleIDStrings;
    std::vector<ARAUtf8String> _factoryCompatibleIDs;
    std::vector<ARAContentType> _factoryAnalyzableTypes;
};

Factory::Factory (Sender& hostCommandsSender, size_t index)
: _hostCommandsSender { hostCommandsSender }
{
    RemoteCaller::CustomDecodeFunction customDecode { [this] (const MessageDecoder& decoder) -> void
        {
            decodeReply (_factory, decoder);

            ARA_VALIDATE_API_ARGUMENT(&_factory, _factory.highestSupportedApiGeneration >= kARAAPIGeneration_2_0_Final);

            _factoryStrings.factoryID = _factory.factoryID;
            _factory.factoryID = _factoryStrings.factoryID.c_str ();

            _factoryStrings.plugInName = _factory.plugInName;
            _factory.plugInName = _factoryStrings.plugInName.c_str ();
            _factoryStrings.manufacturerName = _factory.manufacturerName;
            _factory.manufacturerName = _factoryStrings.manufacturerName.c_str ();
            _factoryStrings.informationURL = _factory.informationURL;
            _factory.informationURL = _factoryStrings.informationURL.c_str ();
            _factoryStrings.version = _factory.version;
            _factory.version = _factoryStrings.version.c_str ();

            _factoryStrings.documentArchiveID = _factory.documentArchiveID;
            _factory.documentArchiveID = _factoryStrings.documentArchiveID.c_str ();

            _factoryCompatibleIDStrings.reserve (_factory.compatibleDocumentArchiveIDsCount);
            _factoryCompatibleIDs.reserve (_factory.compatibleDocumentArchiveIDsCount);
            for (auto i { 0U }; i < _factory.compatibleDocumentArchiveIDsCount; ++i)
            {
                _factoryCompatibleIDStrings.emplace_back (_factory.compatibleDocumentArchiveIDs[i]);
                _factoryCompatibleIDs.emplace_back (_factoryCompatibleIDStrings[i].c_str ());
            }
            _factory.compatibleDocumentArchiveIDs = _factoryCompatibleIDs.data ();

            _factoryAnalyzableTypes.reserve (_factory.analyzeableContentTypesCount);
            for (auto i { 0U }; i < _factory.analyzeableContentTypesCount; ++i)
                _factoryAnalyzableTypes.emplace_back (_factory.analyzeableContentTypes[i]);
            _factory.analyzeableContentTypes = _factoryAnalyzableTypes.data ();
        } };

    RemoteCaller { _hostCommandsSender }.remoteCallWithReply (customDecode, kGetFactoryMessageID, index);
}

std::vector<Factory> _factories {};

size_t initializeFactories (Sender& hostCommandsSender)
{
    size_t count;
    RemoteCaller { hostCommandsSender }.remoteCallWithReply (count, kGetFactoriesCountMessageID);
    ARA_INTERNAL_ASSERT (count > 0);

    _factories.reserve (count);
    for (auto i { 0U }; i < count; ++i)
        _factories.emplace_back (hostCommandsSender, i);
    return _factories.size ();
}

Factory* getFactoryAtIndex (size_t index)
{
    ARA_INTERNAL_ASSERT (index < _factories.size ());
    return &_factories[index];
}

const ARAFactory* getFactoryData (Factory* proxyFactory)
{
    return proxyFactory->getFactory ();
}

const ARADocumentControllerInstance* createDocumentControllerWithDocument (Factory* proxyFactory, const ARADocumentControllerHostInstance* hostInstance, const ARADocumentProperties* properties)
{
    auto result { new DocumentController { proxyFactory->getHostCommandsSender (), proxyFactory->getFactory (), hostInstance, properties} };
    return result->getInstance ();
}

ARADocumentControllerRef getDocumentControllerRemoteRef (ARADocumentControllerRef documentControllerRef)
{
    return static_cast<DocumentController*> (PlugIn::fromRef (documentControllerRef))->getRemoteRef ();
}

const ARAPlugInExtensionInstance* createPlugInExtensionInstance (size_t remoteExtensionRef, Sender& sender, ARADocumentControllerRef documentControllerRef,
                                                                 ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles)
{
    return new PlugInExtension { sender, documentControllerRef, knownRoles, assignedRoles, remoteExtensionRef };
}

void destroyPlugInExtensionInstance (const ARAPlugInExtensionInstance* plugInExtensionInstance)
{
    delete static_cast<const PlugInExtension*> (plugInExtensionInstance);
}

void plugInCallbacksDispatcher (const MessageID messageID, const MessageDecoder& decoder, MessageEncoder* const replyEncoder)
{
//  ARA_LOG ("plugInCallbackDispatcher received message %s", decodeHostMessageID (messageID));

    // ARAAudioAccessControllerInterface
    if (messageID == ARA_IPC_HOST_METHOD_ID (ARAAudioAccessControllerInterface, createAudioReaderForSource))
    {
        ARAAudioAccessControllerHostRef controllerHostRef;
        ARAAudioSourceHostRef audioSourceHostRef;
        ARABool use64BitSamples;
        decodeArguments (decoder, controllerHostRef, audioSourceHostRef, use64BitSamples);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));
        auto audioSource { fromHostRef (audioSourceHostRef) };
        ARA_VALIDATE_API_ARGUMENT (audioSourceHostRef, isValidInstance (audioSource));

        auto reader { new HostAudioReader { audioSource, nullptr, (use64BitSamples != kARAFalse) ? sizeof (double) : sizeof (float) } };
        reader->hostRef = documentController->getHostAudioAccessController ()->createAudioReaderForSource (audioSource->hostRef, (use64BitSamples) ? kARATrue : kARAFalse);
        ARAAudioReaderHostRef audioReaderHostRef { toHostRef (reader) };
        return encodeReply (replyEncoder, audioReaderHostRef);
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAAudioAccessControllerInterface, readAudioSamples))
    {
        ARAAudioAccessControllerHostRef controllerHostRef;
        ARAAudioReaderHostRef audioReaderHostRef;
        ARASamplePosition samplePosition;
        ARASampleCount samplesPerChannel;
        decodeArguments (decoder, controllerHostRef, audioReaderHostRef, samplePosition, samplesPerChannel);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        auto reader { fromHostRef (audioReaderHostRef) };

        // \todo using static (plus not copy bytes) here assumes single-threaded callbacks, but currently this is a valid requirement
        static std::vector<ARAByte> bufferData;
        const auto channelCount { static_cast<size_t> (reader->audioSource->channelCount) };
        const auto bufferSize { reader->sampleSize * static_cast<size_t> (samplesPerChannel) };
        const auto allBuffersSize { channelCount * bufferSize };
        if (bufferData.size () < allBuffersSize)
            bufferData.resize (allBuffersSize);

        static std::vector<void *> sampleBuffers;
        static std::vector<BytesEncoder> encoders;
        if (sampleBuffers.size () < channelCount)
            sampleBuffers.resize (channelCount, nullptr);
        if (encoders.size () < channelCount)
            encoders.resize (channelCount, { nullptr, 0, false });
        for (auto i { 0U }; i < channelCount; ++i)
        {
            const auto buffer { bufferData.data () + i * bufferSize };
            sampleBuffers[i] = buffer;
            encoders[i] = { buffer, bufferSize, false };
        }

        if (documentController->getHostAudioAccessController ()->readAudioSamples (reader->hostRef, samplePosition, samplesPerChannel, sampleBuffers.data ()))
            return encodeReply (replyEncoder, ArrayArgument<const BytesEncoder> { encoders.data (), encoders.size () });
        else
            return; // send empty reply as indication of failure
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAAudioAccessControllerInterface, destroyAudioReader))
    {
        ARAAudioAccessControllerHostRef controllerHostRef;
        ARAAudioReaderHostRef audioReaderHostRef;
        decodeArguments (decoder, controllerHostRef, audioReaderHostRef);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));
        auto reader { fromHostRef (audioReaderHostRef) };

        documentController->getHostAudioAccessController ()->destroyAudioReader (reader->hostRef);
        delete reader;
    }

    // ARAArchivingControllerInterface
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAArchivingControllerInterface, getArchiveSize))
    {
        ARAArchivingControllerHostRef controllerHostRef;
        ARAArchiveReaderHostRef archiveReaderHostRef;
        decodeArguments (decoder, controllerHostRef, archiveReaderHostRef);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        return encodeReply (replyEncoder, documentController->getHostArchivingController ()->getArchiveSize (archiveReaderHostRef));
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAArchivingControllerInterface, readBytesFromArchive))
    {
        ARAArchivingControllerHostRef controllerHostRef;
        ARAArchiveReaderHostRef archiveReaderHostRef;
        ARASize position;
        ARASize length;
        decodeArguments (decoder, controllerHostRef, archiveReaderHostRef, position, length);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        // \todo using static here assumes single-threaded callbacks, but currently this is a valid requirement
        static std::vector<ARAByte> bytes;
        bytes.resize (length);
        if (!documentController->getHostArchivingController ()->readBytesFromArchive (archiveReaderHostRef, position, length, bytes.data ()))
            bytes.clear ();
        return encodeReply (replyEncoder, BytesEncoder { bytes, false });
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAArchivingControllerInterface, writeBytesToArchive))
    {
        ARAArchivingControllerHostRef controllerHostRef;
        ARAArchiveWriterHostRef archiveWriterHostRef;
        ARASize position;
        std::vector<ARAByte> bytes;
        BytesDecoder writer { bytes };
        decodeArguments (decoder, controllerHostRef, archiveWriterHostRef, position, writer);
        ARA_INTERNAL_ASSERT (bytes.size () > 0);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        return encodeReply (replyEncoder, documentController->getHostArchivingController ()->writeBytesToArchive (archiveWriterHostRef, position, bytes.size (), bytes.data ()));
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAArchivingControllerInterface, notifyDocumentArchivingProgress))
    {
        ARAArchivingControllerHostRef controllerHostRef;
        float value;
        decodeArguments (decoder, controllerHostRef, value);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        documentController->getHostArchivingController ()->notifyDocumentArchivingProgress (value);
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAArchivingControllerInterface, notifyDocumentUnarchivingProgress))
    {
        ARAArchivingControllerHostRef controllerHostRef;
        float value;
        decodeArguments (decoder, controllerHostRef, value);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        documentController->getHostArchivingController ()->notifyDocumentUnarchivingProgress (value);
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAArchivingControllerInterface, getDocumentArchiveID))
    {
        ARAArchivingControllerHostRef controllerHostRef;
        ARAArchiveReaderHostRef archiveReaderHostRef;
        decodeArguments (decoder, controllerHostRef, archiveReaderHostRef);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        return encodeReply (replyEncoder, documentController->getHostArchivingController ()->getDocumentArchiveID (archiveReaderHostRef));
    }

    // ARAContentAccessControllerInterface
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, isMusicalContextContentAvailable))
    {
        ARAModelUpdateControllerHostRef controllerHostRef;
        ARAMusicalContextHostRef musicalContextHostRef;
        ARAContentType contentType;
        decodeArguments (decoder, controllerHostRef, musicalContextHostRef, contentType);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        return encodeReply (replyEncoder, (documentController->getHostContentAccessController ()->isMusicalContextContentAvailable (musicalContextHostRef, contentType)) ? kARATrue : kARAFalse);
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, getMusicalContextContentGrade))
    {
        ARAModelUpdateControllerHostRef controllerHostRef;
        ARAMusicalContextHostRef musicalContextHostRef;
        ARAContentType contentType;
        decodeArguments (decoder, controllerHostRef, musicalContextHostRef, contentType);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        return encodeReply (replyEncoder, documentController->getHostContentAccessController ()->getMusicalContextContentGrade (musicalContextHostRef, contentType));
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, createMusicalContextContentReader))
    {
        ARAModelUpdateControllerHostRef controllerHostRef;
        ARAMusicalContextHostRef musicalContextHostRef;
        ARAContentType contentType;
        OptionalArgument<ARAContentTimeRange> range;
        decodeArguments (decoder, controllerHostRef, musicalContextHostRef, contentType, range);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        auto hostContentReader { new HostContentReader };
        hostContentReader->hostRef = documentController->getHostContentAccessController ()->createMusicalContextContentReader (musicalContextHostRef, contentType, (range.second) ? &range.first : nullptr);
        hostContentReader->contentType = contentType;

        return encodeReply (replyEncoder, ARAContentReaderHostRef { toHostRef (hostContentReader) });
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, isAudioSourceContentAvailable))
    {
        ARAModelUpdateControllerHostRef controllerHostRef;
        ARAAudioSourceHostRef audioSourceHostRef;
        ARAContentType contentType;
        decodeArguments (decoder, controllerHostRef, audioSourceHostRef, contentType);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));
        auto audioSource { fromHostRef (audioSourceHostRef) };
        ARA_VALIDATE_API_ARGUMENT (audioSourceHostRef, isValidInstance (audioSource));

        return encodeReply (replyEncoder, (documentController->getHostContentAccessController ()->isAudioSourceContentAvailable (audioSource->hostRef, contentType)) ? kARATrue : kARAFalse);
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, getAudioSourceContentGrade))
    {
        ARAModelUpdateControllerHostRef controllerHostRef;
        ARAAudioSourceHostRef audioSourceHostRef;
        ARAContentType contentType;
        decodeArguments (decoder, controllerHostRef, audioSourceHostRef, contentType);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));
        auto audioSource { fromHostRef (audioSourceHostRef) };
        ARA_VALIDATE_API_ARGUMENT (audioSourceHostRef, isValidInstance (audioSource));

        return encodeReply (replyEncoder, documentController->getHostContentAccessController ()->getAudioSourceContentGrade (audioSource->hostRef, contentType));
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, createAudioSourceContentReader))
    {
        ARAModelUpdateControllerHostRef controllerHostRef;
        ARAAudioSourceHostRef audioSourceHostRef;
        ARAContentType contentType;
        OptionalArgument<ARAContentTimeRange> range;
        decodeArguments (decoder, controllerHostRef, audioSourceHostRef, contentType, range);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));
        auto audioSource { fromHostRef (audioSourceHostRef) };
        ARA_VALIDATE_API_ARGUMENT (audioSourceHostRef, isValidInstance (audioSource));

        auto hostContentReader { new HostContentReader };
        hostContentReader->hostRef = documentController->getHostContentAccessController ()->createAudioSourceContentReader (audioSource->hostRef, contentType, (range.second) ? &range.first : nullptr);
        hostContentReader->contentType = contentType;
        return encodeReply (replyEncoder, ARAContentReaderHostRef { toHostRef (hostContentReader) });
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, getContentReaderEventCount))
    {
        ARAModelUpdateControllerHostRef controllerHostRef;
        ARAContentReaderHostRef contentReaderHostRef;
        decodeArguments (decoder, controllerHostRef, contentReaderHostRef);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));
        auto hostContentReader { fromHostRef (contentReaderHostRef) };

        return encodeReply (replyEncoder, documentController->getHostContentAccessController ()->getContentReaderEventCount (hostContentReader->hostRef));
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, getContentReaderDataForEvent))
    {
        ARAModelUpdateControllerHostRef controllerHostRef;
        ARAContentReaderHostRef contentReaderHostRef;
        ARAInt32 eventIndex;
        decodeArguments (decoder, controllerHostRef, contentReaderHostRef, eventIndex);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));
        auto hostContentReader { fromHostRef (contentReaderHostRef) };

        const void* eventData { documentController->getHostContentAccessController ()->getContentReaderDataForEvent (hostContentReader->hostRef, eventIndex) };
        return encodeContentEvent (replyEncoder, hostContentReader->contentType, eventData);
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, destroyContentReader))
    {
        ARAModelUpdateControllerHostRef controllerHostRef;
        ARAContentReaderHostRef contentReaderHostRef;
        decodeArguments (decoder, controllerHostRef, contentReaderHostRef);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));
        auto hostContentReader { fromHostRef (contentReaderHostRef) };

        documentController->getHostContentAccessController ()->destroyContentReader (hostContentReader->hostRef);
        delete hostContentReader;
    }

    // ARAModelUpdateControllerInterface
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAModelUpdateControllerInterface, notifyAudioSourceAnalysisProgress))
    {
        ARAModelUpdateControllerHostRef controllerHostRef;
        ARAAudioSourceHostRef audioSourceHostRef;
        ARAAnalysisProgressState state;
        float value;
        decodeArguments (decoder, controllerHostRef, audioSourceHostRef, state, value);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));
        auto audioSource { fromHostRef (audioSourceHostRef) };
        ARA_VALIDATE_API_ARGUMENT (audioSourceHostRef, isValidInstance (audioSource));

        documentController->getHostModelUpdateController ()->notifyAudioSourceAnalysisProgress (audioSource->hostRef, state, value);
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAModelUpdateControllerInterface, notifyAudioSourceContentChanged))
    {
        ARAModelUpdateControllerHostRef controllerHostRef;
        ARAAudioSourceHostRef audioSourceHostRef;
        OptionalArgument<ARAContentTimeRange> range;
        ARAContentUpdateFlags scopeFlags;
        decodeArguments (decoder, controllerHostRef, audioSourceHostRef, range, scopeFlags);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));
        auto audioSource { fromHostRef (audioSourceHostRef) };
        ARA_VALIDATE_API_ARGUMENT (audioSourceHostRef, isValidInstance (audioSource));

        documentController->getHostModelUpdateController ()->notifyAudioSourceContentChanged (audioSource->hostRef, (range.second) ? &range.first : nullptr, scopeFlags);
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAModelUpdateControllerInterface, notifyAudioModificationContentChanged))
    {
        ARAModelUpdateControllerHostRef controllerHostRef;
        ARAAudioModificationHostRef audioModificationHostRef;
        OptionalArgument<ARAContentTimeRange> range;
        ARAContentUpdateFlags scopeFlags;
        decodeArguments (decoder, controllerHostRef, audioModificationHostRef, range, scopeFlags);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        documentController->getHostModelUpdateController ()->notifyAudioModificationContentChanged (audioModificationHostRef, (range.second) ? &range.first : nullptr, scopeFlags);
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAModelUpdateControllerInterface, notifyPlaybackRegionContentChanged))
    {
        ARAModelUpdateControllerHostRef controllerHostRef;
        ARAPlaybackRegionHostRef playbackRegionHostRef;
        OptionalArgument<ARAContentTimeRange> range;
        ARAContentUpdateFlags scopeFlags;
        decodeArguments (decoder, controllerHostRef, playbackRegionHostRef, range, scopeFlags);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        documentController->getHostModelUpdateController ()->notifyPlaybackRegionContentChanged (playbackRegionHostRef, (range.second) ? &range.first : nullptr, scopeFlags);
    }

    // ARAPlaybackControllerInterface
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAPlaybackControllerInterface, requestStartPlayback))
    {
        ARAPlaybackControllerHostRef controllerHostRef;
        decodeArguments (decoder, controllerHostRef);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        documentController->getHostPlaybackController ()->requestStartPlayback ();
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAPlaybackControllerInterface, requestStopPlayback))
    {
        ARAPlaybackControllerHostRef controllerHostRef;
        decodeArguments (decoder, controllerHostRef);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        documentController->getHostPlaybackController ()->requestStopPlayback ();
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAPlaybackControllerInterface, requestSetPlaybackPosition))
    {
        ARAPlaybackControllerHostRef controllerHostRef;
        ARATimePosition timePosition;
        decodeArguments (decoder, controllerHostRef, timePosition);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        documentController->getHostPlaybackController ()->requestSetPlaybackPosition (timePosition);
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAPlaybackControllerInterface, requestSetCycleRange))
    {
        ARAPlaybackControllerHostRef controllerHostRef;
        ARATimePosition startTime;
        ARATimeDuration duration;
        decodeArguments (decoder, controllerHostRef, startTime, duration);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        documentController->getHostPlaybackController ()->requestSetCycleRange (startTime, duration);
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAPlaybackControllerInterface, requestEnableCycle))
    {
        ARAPlaybackControllerHostRef controllerHostRef;
        ARABool enable;
        decodeArguments (decoder, controllerHostRef, enable);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        documentController->getHostPlaybackController ()->requestEnableCycle (enable != kARAFalse);
    }
    else
    {
        ARA_INTERNAL_ASSERT (false && "unhandled message ID");
    }

    // all calls that create a reply return early from their respective if ().
// it is valid to provide a dummy replyEncoder if no reply has been requested.
//    ARA_INTERNAL_ASSERT (replyEncoder == nullptr);
}

}   // namespace ProxyPlugIn
}   // namespace IPC
}   // namespace ARA

#endif // ARA_ENABLE_IPC
