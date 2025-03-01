//------------------------------------------------------------------------------
//! \file       ARAIPCProxyPlugIn.cpp
//!             implementation of host-side ARA IPC proxy plug-in
//! \project    ARA SDK Library
//! \copyright  Copyright (c) 2021-2025, Celemony Software GmbH, All Rights Reserved.
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

#include "ARA_Library/IPC/ARAIPCEncoding.h"
#include "ARA_Library/Dispatch/ARAPlugInDispatch.h"
#include "ARA_Library/Dispatch/ARAHostDispatch.h"

#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
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
namespace ProxyPlugInImpl {

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

#if ARA_VALIDATE_API_CALLS
template<typename SubclassT>
std::set<const InstanceValidator<SubclassT>*> InstanceValidator<SubclassT>::_instances;

template<typename SubclassT>
inline bool isValidInstance (const SubclassT* instance)
{
    return InstanceValidator<SubclassT>::isValid (instance);
}
#endif


/*******************************************************************************/

struct AudioSource : public InstanceValidator<AudioSource>
{
    AudioSource (ARAAudioSourceHostRef hostRef, ARAAudioSourceRef remoteRef, ARAChannelCount channelCount
#if ARA_VALIDATE_API_CALLS
                 , ARASampleCount sampleCount, ARASampleRate sampleRate
#endif
                 )
    : _hostRef { hostRef }, _remoteRef { remoteRef }, _channelCount { channelCount }
#if ARA_VALIDATE_API_CALLS
    , _sampleCount { sampleCount }, _sampleRate { sampleRate }
#endif
    {}

    ARAAudioSourceHostRef _hostRef;
    ARAAudioSourceRef _remoteRef;
    ARAChannelCount _channelCount;
#if ARA_VALIDATE_API_CALLS
    ARASampleCount _sampleCount;
    ARASampleRate _sampleRate;
#endif
};
ARA_MAP_REF (AudioSource, ARAAudioSourceRef)
ARA_MAP_HOST_REF (AudioSource, ARAAudioSourceHostRef)

struct ContentReader : public InstanceValidator<ContentReader>
{
    ContentReader (ARAContentReaderRef remoteRef, ARAContentType type)
    : _remoteRef { remoteRef }, _decoder { type }
    {}

    ARAContentReaderRef _remoteRef;
    ContentEventDecoder _decoder;
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

class DocumentController : public PlugIn::DocumentControllerInterface, public RemoteCaller, public InstanceValidator<DocumentController>
{
public:
    DocumentController (Connection* connection, const ARAFactory* factory, const ARADocumentControllerHostInstance* instance, const ARADocumentProperties* properties) noexcept;

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
    bool isAudioModificationPreservingAudioSourceSignal (ARAAudioModificationRef audioModificationRef) noexcept override;
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

DocumentController::DocumentController (Connection* connection, const ARAFactory* factory, const ARADocumentControllerHostInstance* instance, const ARADocumentProperties* properties) noexcept
: RemoteCaller { connection },
  _factory { factory },
  _hostAudioAccessController { instance },
  _hostArchivingController { instance },
  _hostContentAccessController { instance },
  _hostModelUpdateController { instance },
  _hostPlaybackController { instance },
  _instance { this }
{
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    ARAAudioAccessControllerHostRef audioAccessControllerHostRef { toHostRef (this) };
    ARAArchivingControllerHostRef archivingControllerHostRef { toHostRef (this) };
    ARAContentAccessControllerHostRef contentAccessControllerHostRef { toHostRef (this) };
    ARAModelUpdateControllerHostRef modelUpdateControllerHostRef { toHostRef (this) };
    ARAPlaybackControllerHostRef playbackControllerHostRef { toHostRef (this) };
    remoteCall (_remoteRef, kCreateDocumentControllerMethodID, _factory->factoryID,
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
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    ARA_LOG_MODELOBJECT_LIFETIME ("will destroy document controller", _remoteRef);
    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyDocumentController), _remoteRef);

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
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    return _factory;
}

/*******************************************************************************/

void DocumentController::beginEditing () noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, beginEditing), _remoteRef);
}

void DocumentController::endEditing () noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, endEditing), _remoteRef);
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
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    if (!_hostModelUpdateController.isProvided ())
        return;

    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, notifyModelUpdates), _remoteRef);
}

bool DocumentController::restoreObjectsFromArchive (ARAArchiveReaderHostRef archiveReaderHostRef, const ARARestoreObjectsFilter* filter) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    ARABool success;
    remoteCall (success, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, restoreObjectsFromArchive), _remoteRef, archiveReaderHostRef, filter);
    return (success != kARAFalse);
}

bool DocumentController::storeObjectsToArchive (ARAArchiveWriterHostRef archiveWriterHostRef, const ARAStoreObjectsFilter* filter) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    ARAStoreObjectsFilter tempFilter;
    std::vector<ARAAudioSourceRef> remoteAudioSourceRefs;
    if ((filter != nullptr) && (filter->audioSourceRefsCount > 0))
    {
        remoteAudioSourceRefs.reserve (filter->audioSourceRefsCount);
        for (auto i { 0U }; i < filter->audioSourceRefsCount; ++i)
            remoteAudioSourceRefs.emplace_back (fromRef (filter->audioSourceRefs[i])->_remoteRef);

        tempFilter = *filter;
        tempFilter.audioSourceRefs = remoteAudioSourceRefs.data ();
        filter = &tempFilter;
    }

    ARABool success;
    remoteCall (success, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, storeObjectsToArchive), _remoteRef, archiveWriterHostRef, filter);
    return (success!= kARAFalse);
}

bool DocumentController::storeAudioSourceToAudioFileChunk (ARAArchiveWriterHostRef archiveWriterHostRef, ARAAudioSourceRef audioSourceRef, ARAPersistentID* documentArchiveID, bool* openAutomatically) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    auto audioSource { fromRef (audioSourceRef) };
    ARA_INTERNAL_ASSERT (isValidInstance (audioSource));
    ARA_INTERNAL_ASSERT (documentArchiveID != nullptr);
    ARA_INTERNAL_ASSERT (openAutomatically != nullptr);

    bool success { false };
    CustomDecodeFunction customDecode {
        [this, &success, &documentArchiveID, &openAutomatically] (const MessageDecoder* decoder) -> void
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
                ARA_INTERNAL_ASSERT (*documentArchiveID != nullptr);
            }

            *openAutomatically = (reply.openAutomatically != kARAFalse);
            success = (reply.result != kARAFalse);
        } };
    remoteCall (customDecode, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, storeAudioSourceToAudioFileChunk),
                _remoteRef, archiveWriterHostRef, audioSource->_remoteRef);
    return success;
}

void DocumentController::updateDocumentProperties (PropertiesPtr<ARADocumentProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    ARA_INTERNAL_ASSERT (properties != nullptr);
    ARA_INTERNAL_ASSERT (properties->structSize >= ARA::kARADocumentPropertiesMinSize);

    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateDocumentProperties), _remoteRef, *properties);
}

/*******************************************************************************/

ARAMusicalContextRef DocumentController::createMusicalContext (ARAMusicalContextHostRef hostRef, PropertiesPtr<ARAMusicalContextProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    ARA_INTERNAL_ASSERT (properties != nullptr);
    ARA_INTERNAL_ASSERT (properties->structSize >= ARA::kARAMusicalContextPropertiesMinSize);

    ARAMusicalContextRef musicalContextRef;
    remoteCall (musicalContextRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createMusicalContext), _remoteRef, hostRef, *properties);

    ARA_LOG_MODELOBJECT_LIFETIME ("did create musical context", musicalContextRef);
    return musicalContextRef;
}

void DocumentController::updateMusicalContextProperties (ARAMusicalContextRef musicalContextRef, PropertiesPtr<ARAMusicalContextProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (musicalContextRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    ARA_INTERNAL_ASSERT (properties != nullptr);
    ARA_INTERNAL_ASSERT (properties->structSize >= ARA::kARAMusicalContextPropertiesMinSize);

    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateMusicalContextProperties), _remoteRef, musicalContextRef, *properties);
}

void DocumentController::updateMusicalContextContent (ARAMusicalContextRef musicalContextRef, const ARAContentTimeRange* range, ContentUpdateScopes flags) noexcept
{
    ARA_LOG_HOST_ENTRY (musicalContextRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateMusicalContextContent), _remoteRef, musicalContextRef, range, flags);
}

void DocumentController::destroyMusicalContext (ARAMusicalContextRef musicalContextRef) noexcept
{
    ARA_LOG_HOST_ENTRY (musicalContextRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    ARA_LOG_MODELOBJECT_LIFETIME ("will destroy musical context", musicalContextRef);
    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyMusicalContext), _remoteRef, musicalContextRef);
}

/*******************************************************************************/

ARARegionSequenceRef DocumentController::createRegionSequence (ARARegionSequenceHostRef hostRef, PropertiesPtr<ARARegionSequenceProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    ARA_INTERNAL_ASSERT (properties != nullptr);
    ARA_INTERNAL_ASSERT (properties->structSize >= ARA::kARARegionSequencePropertiesMinSize);

    ARARegionSequenceRef regionSequenceRef;
    remoteCall (regionSequenceRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createRegionSequence), _remoteRef, hostRef, *properties);

    ARA_LOG_MODELOBJECT_LIFETIME ("did create region sequence", regionSequenceRef);
    return regionSequenceRef;
}

void DocumentController::updateRegionSequenceProperties (ARARegionSequenceRef regionSequenceRef, PropertiesPtr<ARARegionSequenceProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (regionSequenceRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    ARA_INTERNAL_ASSERT (properties != nullptr);
    ARA_INTERNAL_ASSERT (properties->structSize >= ARA::kARARegionSequencePropertiesMinSize);

    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateRegionSequenceProperties), _remoteRef, regionSequenceRef, *properties);
}

void DocumentController::destroyRegionSequence (ARARegionSequenceRef regionSequenceRef) noexcept
{
    ARA_LOG_HOST_ENTRY (regionSequenceRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    ARA_LOG_MODELOBJECT_LIFETIME ("will destroy region sequence", regionSequenceRef);
    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyRegionSequence), _remoteRef, regionSequenceRef);
}

/*******************************************************************************/

ARAAudioSourceRef DocumentController::createAudioSource (ARAAudioSourceHostRef hostRef, PropertiesPtr<ARAAudioSourceProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    ARA_INTERNAL_ASSERT (properties != nullptr);
    ARA_INTERNAL_ASSERT (properties->structSize >= ARA::kARAAudioSourcePropertiesMinSize);

    auto audioSource { new AudioSource { hostRef, nullptr, properties->channelCount
#if ARA_VALIDATE_API_CALLS
                                                , properties->sampleCount, properties->sampleRate
#endif
                                        } };

    remoteCall (audioSource->_remoteRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createAudioSource),
                _remoteRef, ARAAudioSourceHostRef { toHostRef (audioSource) }, *properties);

    ARA_LOG_MODELOBJECT_LIFETIME ("did create audio source", audioSourceRef);
    return toRef (audioSource);
}

void DocumentController::updateAudioSourceProperties (ARAAudioSourceRef audioSourceRef, PropertiesPtr<ARAAudioSourceProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    auto audioSource { fromRef (audioSourceRef) };
    ARA_INTERNAL_ASSERT (isValidInstance (audioSource));
    ARA_INTERNAL_ASSERT (properties != nullptr);
    ARA_INTERNAL_ASSERT (properties->structSize >= ARA::kARAAudioSourcePropertiesMinSize);

    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateAudioSourceProperties), _remoteRef, audioSource->_remoteRef, *properties);
}

void DocumentController::updateAudioSourceContent (ARAAudioSourceRef audioSourceRef, const ARAContentTimeRange* range, ContentUpdateScopes flags) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    auto audioSource { fromRef (audioSourceRef) };
    ARA_INTERNAL_ASSERT (isValidInstance (audioSource));

    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateAudioSourceContent), _remoteRef, audioSource->_remoteRef, range, flags);
}

void DocumentController::enableAudioSourceSamplesAccess (ARAAudioSourceRef audioSourceRef, bool enable) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_INTERNAL_ASSERT (isValidInstance (audioSource));

    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, enableAudioSourceSamplesAccess), _remoteRef, audioSource->_remoteRef, (enable) ? kARATrue : kARAFalse);
}

void DocumentController::deactivateAudioSourceForUndoHistory (ARAAudioSourceRef audioSourceRef, bool deactivate) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_INTERNAL_ASSERT (isValidInstance (audioSource));

    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, deactivateAudioSourceForUndoHistory), _remoteRef, audioSource->_remoteRef, (deactivate) ? kARATrue : kARAFalse);
}

void DocumentController::destroyAudioSource (ARAAudioSourceRef audioSourceRef) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_INTERNAL_ASSERT (isValidInstance (audioSource));

    ARA_LOG_MODELOBJECT_LIFETIME ("will destroy audio source", audioSource->_remoteRef);
    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyAudioSource), _remoteRef, audioSource->_remoteRef);
    delete audioSource;
}

/*******************************************************************************/

ARAAudioModificationRef DocumentController::createAudioModification (ARAAudioSourceRef audioSourceRef, ARAAudioModificationHostRef hostRef, PropertiesPtr<ARAAudioModificationProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    auto audioSource { fromRef (audioSourceRef) };
    ARA_INTERNAL_ASSERT (isValidInstance (audioSource));
    ARA_INTERNAL_ASSERT (properties != nullptr);
    ARA_INTERNAL_ASSERT (properties->structSize >= ARA::kARAAudioModificationPropertiesMinSize);

    ARAAudioModificationRef audioModificationRef;
    remoteCall (audioModificationRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createAudioModification),
                _remoteRef, audioSource->_remoteRef, hostRef, *properties);

    ARA_LOG_MODELOBJECT_LIFETIME ("did create audio modification", audioModificationRef);
    return audioModificationRef;
}

ARAAudioModificationRef DocumentController::cloneAudioModification (ARAAudioModificationRef srcAudioModificationRef, ARAAudioModificationHostRef hostRef, PropertiesPtr<ARAAudioModificationProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (srcAudioModificationRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    ARA_INTERNAL_ASSERT (properties != nullptr);
    ARA_INTERNAL_ASSERT (properties->structSize >= ARA::kARAAudioModificationPropertiesMinSize);

    ARAAudioModificationRef clonedAudioModificationRef;
    remoteCall (clonedAudioModificationRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, cloneAudioModification),
                _remoteRef, srcAudioModificationRef, hostRef, *properties);

    ARA_LOG_MODELOBJECT_LIFETIME ("did create cloned audio modification", clonedAudioModificationRef);
    return clonedAudioModificationRef;
}

void DocumentController::updateAudioModificationProperties (ARAAudioModificationRef audioModificationRef, PropertiesPtr<ARAAudioModificationProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (audioModificationRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    ARA_INTERNAL_ASSERT (properties != nullptr);
    ARA_INTERNAL_ASSERT (properties->structSize >= ARA::kARAAudioModificationPropertiesMinSize);

    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updateAudioModificationProperties), _remoteRef, audioModificationRef, *properties);
}

bool DocumentController::isAudioModificationPreservingAudioSourceSignal (ARAAudioModificationRef audioModificationRef) noexcept
{
    ARA_LOG_HOST_ENTRY (audioModificationRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    ARABool result;
    remoteCall (result, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, isAudioModificationPreservingAudioSourceSignal), _remoteRef, audioModificationRef);
    return (result != kARAFalse);
}

void DocumentController::deactivateAudioModificationForUndoHistory (ARAAudioModificationRef audioModificationRef, bool deactivate) noexcept
{
    ARA_LOG_HOST_ENTRY (audioModificationRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, deactivateAudioModificationForUndoHistory), _remoteRef, audioModificationRef, (deactivate) ? kARATrue : kARAFalse);
}

void DocumentController::destroyAudioModification (ARAAudioModificationRef audioModificationRef) noexcept
{
    ARA_LOG_HOST_ENTRY (audioModificationRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    ARA_LOG_MODELOBJECT_LIFETIME ("will destroy audio modification", audioModification);
    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyAudioModification), _remoteRef, audioModificationRef);
}

/*******************************************************************************/

ARAPlaybackRegionRef DocumentController::createPlaybackRegion (ARAAudioModificationRef audioModificationRef, ARAPlaybackRegionHostRef hostRef, PropertiesPtr<ARAPlaybackRegionProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_INTERNAL_ASSERT (isValidInstance (this));ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (properties != nullptr);
    ARA_INTERNAL_ASSERT (properties->structSize >= ARA::kARAPlaybackRegionPropertiesMinSize);

    ARAPlaybackRegionRef playbackRegionRef;
    remoteCall (playbackRegionRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createPlaybackRegion),
                _remoteRef, audioModificationRef, hostRef, *properties);

    ARA_LOG_MODELOBJECT_LIFETIME ("did create playback region", playbackRegionRef);
    return playbackRegionRef;
}

void DocumentController::updatePlaybackRegionProperties (ARAPlaybackRegionRef playbackRegionRef, PropertiesPtr<ARAPlaybackRegionProperties> properties) noexcept
{
    ARA_LOG_HOST_ENTRY (playbackRegionRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    ARA_INTERNAL_ASSERT (properties != nullptr);
    ARA_INTERNAL_ASSERT (properties->structSize >= ARA::kARAPlaybackRegionPropertiesMinSize);

    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, updatePlaybackRegionProperties), _remoteRef, playbackRegionRef, *properties);
}

void DocumentController::getPlaybackRegionHeadAndTailTime (ARAPlaybackRegionRef playbackRegionRef, ARATimeDuration* headTime, ARATimeDuration* tailTime) noexcept
{
    ARA_LOG_HOST_ENTRY (playbackRegionRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
// this function can be called from other threads!
//  ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    ARA_INTERNAL_ASSERT (headTime != nullptr);
    ARA_INTERNAL_ASSERT (tailTime != nullptr);

    GetPlaybackRegionHeadAndTailTimeReply reply;
    remoteCall (reply, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getPlaybackRegionHeadAndTailTime),
                _remoteRef, playbackRegionRef, (headTime != nullptr) ? kARATrue : kARAFalse, (tailTime != nullptr) ? kARATrue : kARAFalse);
    if (headTime != nullptr)
        *headTime = reply.headTime;
    if (tailTime != nullptr)
        *tailTime = reply.tailTime;
}

void DocumentController::destroyPlaybackRegion (ARAPlaybackRegionRef playbackRegionRef) noexcept
{
    ARA_LOG_HOST_ENTRY (playbackRegionRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    ARA_LOG_MODELOBJECT_LIFETIME ("will destroy playback region", playbackRegionRef);
    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyPlaybackRegion), _remoteRef, playbackRegionRef);
}

/*******************************************************************************/

bool DocumentController::isAudioSourceContentAvailable (ARAAudioSourceRef audioSourceRef, ARAContentType type) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));    const auto audioSource { fromRef (audioSourceRef) };
    ARA_INTERNAL_ASSERT (isValidInstance (audioSource));

    ARABool result;
    remoteCall (result, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, isAudioSourceContentAvailable), _remoteRef, audioSource->_remoteRef, type);
    return (result != kARAFalse);
}

ARAContentGrade DocumentController::getAudioSourceContentGrade (ARAAudioSourceRef audioSourceRef, ARAContentType type) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_INTERNAL_ASSERT (isValidInstance (audioSource));

    ARAContentGrade grade;
    remoteCall (grade, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getAudioSourceContentGrade), _remoteRef, audioSource->_remoteRef, type);
    return grade;
}

ARAContentReaderRef DocumentController::createAudioSourceContentReader (ARAAudioSourceRef audioSourceRef, ARAContentType type, const ARAContentTimeRange* range) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_INTERNAL_ASSERT (isValidInstance (audioSource));

    ARAContentReaderRef contentReaderRef;
    remoteCall (contentReaderRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createAudioSourceContentReader),
                _remoteRef, audioSource->_remoteRef, type, range);

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
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    ARABool result;
    remoteCall (result, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, isAudioModificationContentAvailable), _remoteRef, audioModificationRef, type);
    return (result != kARAFalse);
}

ARAContentGrade DocumentController::getAudioModificationContentGrade (ARAAudioModificationRef audioModificationRef, ARAContentType type) noexcept
{
    ARA_LOG_HOST_ENTRY (audioModificationRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    ARAContentGrade grade;
    remoteCall (grade, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getAudioModificationContentGrade), _remoteRef, audioModificationRef, type);
    return grade;
}

ARAContentReaderRef DocumentController::createAudioModificationContentReader (ARAAudioModificationRef audioModificationRef, ARAContentType type, const ARAContentTimeRange* range) noexcept
{
    ARA_LOG_HOST_ENTRY (audioModificationRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    ARAContentReaderRef contentReaderRef;
    remoteCall (contentReaderRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createAudioModificationContentReader),
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
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    ARABool result;
    remoteCall (result, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, isPlaybackRegionContentAvailable), _remoteRef, playbackRegionRef, type);
    return (result != kARAFalse);
}

ARAContentGrade DocumentController::getPlaybackRegionContentGrade (ARAPlaybackRegionRef playbackRegionRef, ARAContentType type) noexcept
{
    ARA_LOG_HOST_ENTRY (playbackRegionRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    ARAContentGrade grade;
    remoteCall (grade, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getPlaybackRegionContentGrade), _remoteRef, playbackRegionRef, type);
    return grade;
}

ARAContentReaderRef DocumentController::createPlaybackRegionContentReader (ARAPlaybackRegionRef playbackRegionRef, ARAContentType type, const ARAContentTimeRange* range) noexcept
{
    ARA_LOG_HOST_ENTRY (playbackRegionRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    ARAContentReaderRef contentReaderRef;
    remoteCall (contentReaderRef, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, createPlaybackRegionContentReader),
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
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    const auto contentReader { fromRef (contentReaderRef) };
    ARA_INTERNAL_ASSERT (isValidInstance (contentReader));

    ARAInt32 count;
    remoteCall (count, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getContentReaderEventCount), _remoteRef, contentReader->_remoteRef);
    return count;
}

const void* DocumentController::getContentReaderDataForEvent (ARAContentReaderRef contentReaderRef, ARAInt32 eventIndex) noexcept
{
    ARA_LOG_HOST_ENTRY (contentReaderRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    const auto contentReader { fromRef (contentReaderRef) };
    ARA_INTERNAL_ASSERT (isValidInstance (contentReader));

    const void* result {};
    CustomDecodeFunction customDecode {
        [&result, &contentReader] (const MessageDecoder* decoder) -> void
        {
            result = contentReader->_decoder.decode (decoder);
        } };
    remoteCall (customDecode, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getContentReaderDataForEvent),
                _remoteRef, contentReader->_remoteRef, eventIndex);
    return result;
}

void DocumentController::destroyContentReader (ARAContentReaderRef contentReaderRef) noexcept
{
    ARA_LOG_HOST_ENTRY (contentReaderRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    const auto contentReader { fromRef (contentReaderRef) };
    ARA_INTERNAL_ASSERT (isValidInstance (contentReader));

    ARA_LOG_MODELOBJECT_LIFETIME ("will destroy content reader", contentReader->remoteRef);
    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, destroyContentReader), _remoteRef, contentReader->_remoteRef);

    delete contentReader;
}

/*******************************************************************************/

bool DocumentController::isAudioSourceContentAnalysisIncomplete (ARAAudioSourceRef audioSourceRef, ARAContentType type) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_INTERNAL_ASSERT (isValidInstance (audioSource));

    ARABool result;
    remoteCall (result, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, isAudioSourceContentAnalysisIncomplete),
                _remoteRef, audioSource->_remoteRef, type);
    return (result != kARAFalse);
}

void DocumentController::requestAudioSourceContentAnalysis (ARAAudioSourceRef audioSourceRef, ARASize contentTypesCount, const ARAContentType contentTypes[]) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_INTERNAL_ASSERT (isValidInstance (audioSource));

    const ArrayArgument<const ARAContentType> types { contentTypes, contentTypesCount };
    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, requestAudioSourceContentAnalysis), _remoteRef, audioSource->_remoteRef, types);
}

ARAInt32 DocumentController::getProcessingAlgorithmsCount () noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    ARAInt32 count;
    remoteCall (count, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getProcessingAlgorithmsCount), _remoteRef);
    return count;
}

const ARAProcessingAlgorithmProperties* DocumentController::getProcessingAlgorithmProperties (ARAInt32 algorithmIndex) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    CustomDecodeFunction customDecode {
        [this] (const MessageDecoder* decoder) -> void
        {
            ARAProcessingAlgorithmProperties reply;
            decodeReply (reply, decoder);
            _processingAlgorithmStrings.persistentID = reply.persistentID;
            _processingAlgorithmStrings.name = reply.name;
            _processingAlgorithmData = reply;
            _processingAlgorithmData.persistentID = _processingAlgorithmStrings.persistentID.c_str ();
            _processingAlgorithmData.name = _processingAlgorithmStrings.name.c_str ();
        } };
    remoteCall (customDecode, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getProcessingAlgorithmProperties), _remoteRef, algorithmIndex);
    return &_processingAlgorithmData;
}

ARAInt32 DocumentController::getProcessingAlgorithmForAudioSource (ARAAudioSourceRef audioSourceRef) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_INTERNAL_ASSERT (isValidInstance (audioSource));

    ARAInt32 result;
    remoteCall (result, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, getProcessingAlgorithmForAudioSource), _remoteRef, audioSource->_remoteRef);
    return result;
}

void DocumentController::requestProcessingAlgorithmForAudioSource (ARAAudioSourceRef audioSourceRef, ARAInt32 algorithmIndex) noexcept
{
    ARA_LOG_HOST_ENTRY (audioSourceRef);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
    const auto audioSource { fromRef (audioSourceRef) };
    ARA_INTERNAL_ASSERT (isValidInstance (audioSource));

    remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, requestProcessingAlgorithmForAudioSource), _remoteRef, audioSource->_remoteRef, algorithmIndex);
}

/*******************************************************************************/

bool DocumentController::isLicensedForCapabilities (bool runModalActivationDialogIfNeeded, ARASize contentTypesCount, const ARAContentType contentTypes[], ARAPlaybackTransformationFlags transformationFlags) noexcept
{
    ARA_LOG_HOST_ENTRY (this);
    ARA_INTERNAL_ASSERT (isValidInstance (this));
    ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

    const ArrayArgument<const ARAContentType> types { contentTypes, contentTypesCount };
    ARABool result;
    remoteCall (result, ARA_IPC_PLUGIN_METHOD_ID (ARADocumentControllerInterface, isLicensedForCapabilities),
                _remoteRef, (runModalActivationDialogIfNeeded) ? kARATrue : kARAFalse, types, transformationFlags);
    return (result != kARAFalse);
}


/*******************************************************************************/
// Implementation of PlaybackRendererInterface that channels all calls through IPC

class PlaybackRenderer : public PlugIn::PlaybackRendererInterface, protected RemoteCaller, public InstanceValidator<PlaybackRenderer>
{
public:
    explicit PlaybackRenderer (Connection* connection, ARAPlaybackRendererRef remoteRef) noexcept
    : RemoteCaller { connection },
      _remoteRef { remoteRef }
    {}

    // Inherited public interface used by the C++ dispatcher, to be called by the ARAPlugInDispatch code exclusively.
    void addPlaybackRegion (ARAPlaybackRegionRef playbackRegionRef) noexcept override
    {
        ARA_LOG_HOST_ENTRY (this);
        ARA_INTERNAL_ASSERT (isValidInstance (this));
        ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

        remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARAPlaybackRendererInterface, addPlaybackRegion), _remoteRef, playbackRegionRef);
    }
    void removePlaybackRegion (ARAPlaybackRegionRef playbackRegionRef) noexcept override
    {
        ARA_LOG_HOST_ENTRY (this);
        ARA_INTERNAL_ASSERT (isValidInstance (this));
        ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

        remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARAPlaybackRendererInterface, removePlaybackRegion), _remoteRef, playbackRegionRef);
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
    explicit EditorRenderer (Connection* connection, ARAEditorRendererRef remoteRef) noexcept
    : RemoteCaller { connection },
      _remoteRef { remoteRef }
    {}

    // Inherited public interface used by the C++ dispatcher, to be called by the ARAPlugInDispatch code exclusively.
    void addPlaybackRegion (ARAPlaybackRegionRef playbackRegionRef) noexcept override
    {
        ARA_LOG_HOST_ENTRY (this);
        ARA_INTERNAL_ASSERT (isValidInstance (this));
        ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

        remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARAEditorRendererInterface, addPlaybackRegion), _remoteRef, playbackRegionRef);
    }
    void removePlaybackRegion (ARAPlaybackRegionRef playbackRegionRef) noexcept override
    {
        ARA_LOG_HOST_ENTRY (this);
        ARA_INTERNAL_ASSERT (isValidInstance (this));
        ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

        remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARAEditorRendererInterface, removePlaybackRegion), _remoteRef, playbackRegionRef);
    }

    void addRegionSequence (ARARegionSequenceRef regionSequenceRef) noexcept override
    {
        ARA_LOG_HOST_ENTRY (this);
        ARA_INTERNAL_ASSERT (isValidInstance (this));
        ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

        remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARAEditorRendererInterface, addRegionSequence), _remoteRef, regionSequenceRef);
    }
    void removeRegionSequence (ARARegionSequenceRef regionSequenceRef) noexcept override
    {
        ARA_LOG_HOST_ENTRY (this);
        ARA_INTERNAL_ASSERT (isValidInstance (this));
        ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

        remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARAEditorRendererInterface, removeRegionSequence), _remoteRef, regionSequenceRef);
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
    explicit EditorView (Connection* connection, ARAEditorViewRef remoteRef) noexcept
    : RemoteCaller { connection },
      _remoteRef { remoteRef }
    {}

    // Inherited public interface used by the C++ dispatcher, to be called by the ARAPlugInDispatch code exclusively.
    void notifySelection (SizedStructPtr<ARAViewSelection> selection) noexcept override
    {
        ARA_LOG_HOST_ENTRY (this);
        ARA_INTERNAL_ASSERT (isValidInstance (this));
        ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());
        ARA_INTERNAL_ASSERT (selection != nullptr);
        ARA_INTERNAL_ASSERT (selection->structSize >= ARA::kARAViewSelectionMinSize);

        remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARAEditorViewInterface, notifySelection), _remoteRef, *selection);
    }
    void notifyHideRegionSequences (ARASize regionSequenceRefsCount, const ARARegionSequenceRef regionSequenceRefs[]) noexcept override
    {
        ARA_LOG_HOST_ENTRY (this);
        ARA_INTERNAL_ASSERT (isValidInstance (this));
        ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

        const ArrayArgument<const ARARegionSequenceRef> sequences { regionSequenceRefs, regionSequenceRefsCount };
        remoteCall (ARA_IPC_PLUGIN_METHOD_ID (ARAEditorViewInterface, notifyHideRegionSequences), _remoteRef, sequences);
    }

private:
    ARAEditorViewRef const _remoteRef;

    ARA_HOST_MANAGED_OBJECT (EditorView)
};


/*******************************************************************************/
// implementation of ARAPlugInExtensionInstance that uses the above instance role classes

class PlugInExtension : public PlugIn::PlugInExtensionInstance, public RemoteCaller
{
public:
    PlugInExtension (Connection* connection, ARAPlugInExtensionRef remoteExtensionRef,
                     ARADocumentControllerRef documentControllerRef,
                     ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles) noexcept
    : PlugIn::PlugInExtensionInstance { (((knownRoles & kARAPlaybackRendererRole) == 0) || ((assignedRoles & kARAPlaybackRendererRole) != 0)) ?
                                            new PlaybackRenderer (connection, reinterpret_cast<ARAPlaybackRendererRef> (remoteExtensionRef)) : nullptr,
                                        (((knownRoles & kARAEditorRendererRole) == 0) || ((assignedRoles & kARAEditorRendererRole) != 0)) ?
                                            new EditorRenderer (connection, reinterpret_cast<ARAEditorRendererRef> (remoteExtensionRef)) : nullptr,
                                        (((knownRoles & kARAEditorViewRole) == 0) || ((assignedRoles & kARAEditorViewRole) != 0)) ?
                                            new EditorView (connection, reinterpret_cast<ARAEditorViewRef> (remoteExtensionRef)) : nullptr },
      RemoteCaller { connection },
      _documentController { PlugIn::fromRef<DocumentController> (documentControllerRef) }
    {
        plugInExtensionRef = remoteExtensionRef;    // we re-use this deprecated ivar to store the remote extension

        ARA_LOG_HOST_ENTRY (this);
        ARA_INTERNAL_ASSERT (isValidInstance (_documentController));
        ARA_INTERNAL_ASSERT (Connection::currentThreadActsAsMainThread ());

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

        remoteCall (kCleanupBindingMethodID, plugInExtensionRef);

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

struct RemoteFactory
{
    ARAFactory _factory;
    struct
    {
        std::string factoryID;
        std::string plugInName;
        std::string manufacturerName;
        std::string informationURL;
        std::string version;
        std::string documentArchiveID;
    } _strings;
    std::vector<std::string> _compatibleIDStrings;
    std::vector<ARAUtf8String> _compatibleIDs;
    std::vector<ARAContentType> _analyzableTypes;
};

std::map<std::string, RemoteFactory> _factories {};


class DistributedMainThreadLock
{
private:
// for some reason, Xcode thinks this is no constant expression...
//  static constexpr ARAIPCConnectionRef _dummyLocalConnectionRef { reinterpret_cast<ARAIPCConnectionRef> (-1) };
    const ARAIPCConnectionRef _dummyLocalConnectionRef { reinterpret_cast<ARAIPCConnectionRef> (-1) };

    template<bool tryLock>
    bool _lockImpl (ARAIPCConnectionRef connectionRef)
    {
        std::unique_lock <std::mutex> lock { _lock };
        if (_lockingConnection == nullptr)
        {
            ARA_INTERNAL_ASSERT (_recursionCount == 0);
            _lockingConnection = connectionRef;
            return true;
        }
        else if (_lockingConnection == connectionRef)
        {
            ++_recursionCount;
            return true;
        }
        else
        {
            if (tryLock)
                return false;

            _condition.wait (lock, [this, connectionRef]
                        { return (_lockingConnection == nullptr) || (_lockingConnection == connectionRef); });
            if (_lockingConnection == nullptr)
            {
                ARA_INTERNAL_ASSERT (_recursionCount == 0);
                _lockingConnection = connectionRef;
            }
            else
            {
                ARA_INTERNAL_ASSERT (_lockingConnection == connectionRef);
                ++_recursionCount;
            }
            return true;
        }
    }

    void _unlockImpl (ARAIPCConnectionRef connectionRef)
    {
        std::unique_lock <std::mutex> lock { _lock };
        ARA_INTERNAL_ASSERT (_lockingConnection == connectionRef);
        if (_recursionCount > 0)
        {
            --_recursionCount;
        }
        else
        {
            _lockingConnection = nullptr;
            _condition.notify_all ();
        }
    }

public:
    DistributedMainThreadLock () = default;

    void lockFromLocalMainThread ()
    {
        _lockImpl<false> (_dummyLocalConnectionRef);
        ARA_IPC_LOG ("distributed main thread was locked from host.");
    }
    bool tryLockFromLocalMainThread ()
    {
        const auto result { _lockImpl<true> (_dummyLocalConnectionRef) };
        if (result)
            ARA_IPC_LOG ("distributed main thread was try-locked from host.");
        else
            ARA_IPC_LOG ("distributed main thread try-lock from host failed.");
        return result;
    }
    void unlockFromLocalMainThread ()
    {
        ARA_IPC_LOG ("distributed main thread will be unlocked from host.");
        _unlockImpl (_dummyLocalConnectionRef);
    }

    void lockFromRemoteMainThread (ARAIPCConnectionRef connectionRef)
    {
        _lockImpl<false> (connectionRef);
        ARA_IPC_LOG ("distributed main thread was locked from remote.");
    }
    bool tryLockFromRemoteMainThread (ARAIPCConnectionRef connectionRef)
    {
        const auto result { _lockImpl<true> (connectionRef) };
        if (result)
            ARA_IPC_LOG ("distributed main thread was try-locked from remote.");
        else
            ARA_IPC_LOG ("distributed main thread try-lock from remote failed.");
        return result;
    }
    void unlockFromRemoteMainThread (ARAIPCConnectionRef connectionRef)
    {
        ARA_IPC_LOG ("distributed main thread will be unlocked from remote.");
        _unlockImpl (connectionRef);
    }

private:
    std::mutex _lock;
    std::condition_variable _condition;
    ARAIPCConnectionRef _lockingConnection { nullptr };
    int _recursionCount { 0 };
} _distributedMainThreadLock {};


/*******************************************************************************/

}   // namespace ProxyPlugInImpl
using namespace ProxyPlugInImpl;

/*******************************************************************************/

#if defined (__GNUC__)
    _Pragma ("GCC diagnostic push")
    _Pragma ("GCC diagnostic ignored \"-Wunused-function\"")
#endif

ARA_MAP_IPC_REF (Connection, ARAIPCConnectionRef)

#if defined (__GNUC__)
    _Pragma ("GCC diagnostic pop")
#endif


size_t ARAIPCProxyPlugInGetFactoriesCount (ARAIPCConnectionRef connectionRef)
{
    size_t count;
    RemoteCaller { fromIPCRef (connectionRef) }.remoteCall (count, kGetFactoriesCountMethodID);
    ARA_INTERNAL_ASSERT (count > 0);
    return count;
}

const ARAFactory* ARAIPCProxyPlugInGetFactoryAtIndex (ARAIPCConnectionRef connectionRef, size_t index)
{
    RemoteFactory remoteFactory;
    RemoteCaller::CustomDecodeFunction customDecode {
        [&remoteFactory] (const MessageDecoder* decoder) -> void
        {
            decodeReply (remoteFactory._factory, decoder);

            ARA_VALIDATE_API_ARGUMENT (&remoteFactory._factory, remoteFactory._factory.highestSupportedApiGeneration >= kARAAPIGeneration_2_0_Final);

            remoteFactory._strings.factoryID = remoteFactory._factory.factoryID;
            remoteFactory._factory.factoryID = remoteFactory._strings.factoryID.c_str ();

            remoteFactory._strings.plugInName = remoteFactory._factory.plugInName;
            remoteFactory._factory.plugInName = remoteFactory._strings.plugInName.c_str ();
            remoteFactory._strings.manufacturerName = remoteFactory._factory.manufacturerName;
            remoteFactory._factory.manufacturerName = remoteFactory._strings.manufacturerName.c_str ();
            remoteFactory._strings.informationURL = remoteFactory._factory.informationURL;
            remoteFactory._factory.informationURL = remoteFactory._strings.informationURL.c_str ();
            remoteFactory._strings.version = remoteFactory._factory.version;
            remoteFactory._factory.version = remoteFactory._strings.version.c_str ();

            remoteFactory._strings.documentArchiveID = remoteFactory._factory.documentArchiveID;
            remoteFactory._factory.documentArchiveID = remoteFactory._strings.documentArchiveID.c_str ();

            remoteFactory._compatibleIDStrings.reserve (remoteFactory._factory.compatibleDocumentArchiveIDsCount);
            remoteFactory._compatibleIDs.reserve (remoteFactory._factory.compatibleDocumentArchiveIDsCount);
            for (auto i { 0U }; i < remoteFactory._factory.compatibleDocumentArchiveIDsCount; ++i)
            {
                remoteFactory._compatibleIDStrings.emplace_back (remoteFactory._factory.compatibleDocumentArchiveIDs[i]);
                remoteFactory._compatibleIDs.emplace_back (remoteFactory._compatibleIDStrings[i].c_str ());
            }
            remoteFactory._factory.compatibleDocumentArchiveIDs = remoteFactory._compatibleIDs.data ();

            remoteFactory._analyzableTypes.reserve (remoteFactory._factory.analyzeableContentTypesCount);
            for (auto i { 0U }; i < remoteFactory._factory.analyzeableContentTypesCount; ++i)
                remoteFactory._analyzableTypes.emplace_back (remoteFactory._factory.analyzeableContentTypes[i]);
            remoteFactory._factory.analyzeableContentTypes = remoteFactory._analyzableTypes.data ();
        } };

    RemoteCaller { fromIPCRef (connectionRef) }.remoteCall (customDecode, kGetFactoryMethodID, index);

    const auto result { _factories.insert (std::make_pair (remoteFactory._strings.factoryID, remoteFactory)) };
    if (result.second)
    {
        result.first->second._factory.factoryID = result.first->second._strings.factoryID.c_str ();

        result.first->second._factory.factoryID = result.first->second._strings.factoryID.c_str ();
        result.first->second._factory.plugInName = result.first->second._strings.plugInName.c_str ();
        result.first->second._factory.manufacturerName = result.first->second._strings.manufacturerName.c_str ();
        result.first->second._factory.informationURL = result.first->second._strings.informationURL.c_str ();
        result.first->second._factory.version = result.first->second._strings.version.c_str ();

        result.first->second._factory.documentArchiveID = result.first->second._strings.documentArchiveID.c_str ();

        for (auto i { 0U }; i < result.first->second._compatibleIDStrings.size (); ++i)
            result.first->second._compatibleIDs[i] = result.first->second._compatibleIDStrings[i].c_str ();
        result.first->second._factory.compatibleDocumentArchiveIDs = result.first->second._compatibleIDs.data ();

        result.first->second._factory.analyzeableContentTypes = result.first->second._analyzableTypes.data ();
    }
    return &result.first->second._factory;
}

void ARAIPCProxyPlugInInitializeARA (ARAIPCConnectionRef connectionRef, const ARAPersistentID factoryID, ARAAPIGeneration desiredApiGeneration)
{
    ARA_INTERNAL_ASSERT (desiredApiGeneration >= kARAAPIGeneration_2_0_Final);
    RemoteCaller { fromIPCRef (connectionRef) }.remoteCall (kInitializeARAMethodID, factoryID, desiredApiGeneration);
}

const ARADocumentControllerInstance* ARAIPCProxyPlugInCreateDocumentControllerWithDocument (
                                            ARAIPCConnectionRef connectionRef, const ARAPersistentID factoryID,
                                            const ARADocumentControllerHostInstance* hostInstance, const ARADocumentProperties* properties)
{
    const auto cached { _factories.find (std::string { factoryID }) };
    ARA_INTERNAL_ASSERT (cached != _factories.end ());
    if (cached == _factories.end ())
        return nullptr;

    auto result { new DocumentController { fromIPCRef (connectionRef), &cached->second._factory, hostInstance, properties } };
    return result->getInstance ();
}

const ARAPlugInExtensionInstance* ARAIPCProxyPlugInBindToDocumentController (ARAIPCPlugInInstanceRef remoteRef, ARADocumentControllerRef documentControllerRef,
                                                                             ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles)
{
    auto documentController { static_cast<DocumentController*> (PlugIn::fromRef (documentControllerRef)) };
    const auto remoteDocumentControllerRef { documentController->getRemoteRef () };

    size_t remoteExtensionRef {};
    documentController->remoteCall (remoteExtensionRef, kBindToDocumentControllerMethodID, remoteRef, remoteDocumentControllerRef, knownRoles, assignedRoles);

    return new PlugInExtension { documentController->getConnection (), reinterpret_cast<ARAPlugInExtensionRef> (remoteExtensionRef), documentControllerRef, knownRoles, assignedRoles };
}

void ARAIPCProxyPlugInCleanupBinding (const ARAPlugInExtensionInstance* plugInExtensionInstance)
{
    delete static_cast<const PlugInExtension*> (plugInExtensionInstance);
}

void ARAIPCProxyPlugInUninitializeARA (ARAIPCConnectionRef connectionRef, const ARAPersistentID factoryID)
{
    RemoteCaller { fromIPCRef (connectionRef) }.remoteCall (kUninitializeARAMethodID, factoryID);
}

ARABool ARAIPCProxyPlugInCurrentThreadActsAsMainThread ()
{
    return (Connection::currentThreadActsAsMainThread ()) ? kARATrue : kARAFalse;
}

void ARAIPCProxyPlugInLockDistributedMainThread ()
{
    _distributedMainThreadLock.lockFromLocalMainThread ();
}

ARABool ARAIPCProxyPlugInTryLockDistributedMainThread ()
{
    return (_distributedMainThreadLock.tryLockFromLocalMainThread ()) ? kARATrue : kARAFalse;
}

void ARAIPCProxyPlugInUnlockDistributedMainThread ()
{
    _distributedMainThreadLock.unlockFromLocalMainThread ();
}

/*******************************************************************************/

void ProxyPlugIn::handleReceivedMessage (const MessageID messageID, const MessageDecoder* const decoder,
                                         MessageEncoder* const replyEncoder)
{
    ARA_IPC_LOG ("ProxyPlugIn handles '%s'", decodePlugInMessageID (messageID));

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
        reader->hostRef = documentController->getHostAudioAccessController ()->createAudioReaderForSource (audioSource->_hostRef, (use64BitSamples) ? kARATrue : kARAFalse);
        ARAAudioReaderHostRef audioReaderHostRef { toHostRef (reader) };
        encodeReply (replyEncoder, audioReaderHostRef);
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
        const auto channelCount { static_cast<size_t> (reader->audioSource->_channelCount) };
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
            encodeReply (replyEncoder, ArrayArgument<const BytesEncoder> { encoders.data (), encoders.size () });
        // else send empty reply as indication of failure
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

        encodeReply (replyEncoder, documentController->getHostArchivingController ()->getArchiveSize (archiveReaderHostRef));
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
        encodeReply (replyEncoder, BytesEncoder { bytes, false });
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

        encodeReply (replyEncoder, documentController->getHostArchivingController ()->writeBytesToArchive (archiveWriterHostRef, position, bytes.size (), bytes.data ()));
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

        encodeReply (replyEncoder, documentController->getHostArchivingController ()->getDocumentArchiveID (archiveReaderHostRef));
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

        encodeReply (replyEncoder, (documentController->getHostContentAccessController ()->isMusicalContextContentAvailable (musicalContextHostRef, contentType)) ? kARATrue : kARAFalse);
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, getMusicalContextContentGrade))
    {
        ARAModelUpdateControllerHostRef controllerHostRef;
        ARAMusicalContextHostRef musicalContextHostRef;
        ARAContentType contentType;
        decodeArguments (decoder, controllerHostRef, musicalContextHostRef, contentType);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        encodeReply (replyEncoder, documentController->getHostContentAccessController ()->getMusicalContextContentGrade (musicalContextHostRef, contentType));
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

        encodeReply (replyEncoder, ARAContentReaderHostRef { toHostRef (hostContentReader) });
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

        encodeReply (replyEncoder, (documentController->getHostContentAccessController ()->isAudioSourceContentAvailable (audioSource->_hostRef, contentType)) ? kARATrue : kARAFalse);
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

        encodeReply (replyEncoder, documentController->getHostContentAccessController ()->getAudioSourceContentGrade (audioSource->_hostRef, contentType));
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
        hostContentReader->hostRef = documentController->getHostContentAccessController ()->createAudioSourceContentReader (audioSource->_hostRef, contentType, (range.second) ? &range.first : nullptr);
        hostContentReader->contentType = contentType;
        encodeReply (replyEncoder, ARAContentReaderHostRef { toHostRef (hostContentReader) });
    }
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAContentAccessControllerInterface, getContentReaderEventCount))
    {
        ARAModelUpdateControllerHostRef controllerHostRef;
        ARAContentReaderHostRef contentReaderHostRef;
        decodeArguments (decoder, controllerHostRef, contentReaderHostRef);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));
        auto hostContentReader { fromHostRef (contentReaderHostRef) };

        encodeReply (replyEncoder, documentController->getHostContentAccessController ()->getContentReaderEventCount (hostContentReader->hostRef));
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
        encodeContentEvent (replyEncoder, hostContentReader->contentType, eventData);
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

        documentController->getHostModelUpdateController ()->notifyAudioSourceAnalysisProgress (audioSource->_hostRef, state, value);
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

        documentController->getHostModelUpdateController ()->notifyAudioSourceContentChanged (audioSource->_hostRef, (range.second) ? &range.first : nullptr, scopeFlags);
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
    else if (messageID == ARA_IPC_HOST_METHOD_ID (ARAModelUpdateControllerInterface, notifyDocumentDataChanged))
    {
        ARAModelUpdateControllerHostRef controllerHostRef;
        decodeArguments (decoder, controllerHostRef);

        auto documentController { fromHostRef (controllerHostRef) };
        ARA_VALIDATE_API_ARGUMENT (controllerHostRef, isValidInstance (documentController));

        documentController->getHostModelUpdateController ()->notifyDocumentDataChanged ();
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
    else if (messageID == kLockDistributedMainThreadMethodID)
    {
        _distributedMainThreadLock.lockFromRemoteMainThread (toIPCRef (getConnection ()));
    }
    else if (messageID == kTryLockDistributedMainThreadMethodID)
    {
        auto result { _distributedMainThreadLock.tryLockFromRemoteMainThread (toIPCRef (getConnection ())) };
        encodeReply (replyEncoder, (result) ? kARATrue : kARAFalse);
    }
    else if (messageID == kUnlockDistributedMainThreadMethodID)
    {
        _distributedMainThreadLock.unlockFromRemoteMainThread (toIPCRef (getConnection ()));
    }
    else
    {
        ARA_INTERNAL_ASSERT (false && "unhandled message ID");
    }
}

}   // namespace IPC
}   // namespace ARA

#endif // ARA_ENABLE_IPC
