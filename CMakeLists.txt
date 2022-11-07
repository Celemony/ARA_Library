# ======================
# ARA Library CMake Integration
#
# Copyright (c) 2020-2022, Celemony Software GmbH, All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ======================

cmake_minimum_required(VERSION 3.4 FATAL_ERROR)

if(ARA_API_DIR)
    # normalize path if provided (and if relative path, assume it's relative to the top-level project)
    get_filename_component(ARA_API_DIR "${ARA_API_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
else()
    # if no path provided, assume ARA_API is right next to our ARA_Library directory
    get_filename_component(ARA_API_DIR ../ARA_API ABSOLUTE "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
set(ARA_API_DIR "${ARA_API_DIR}" CACHE PATH "directory of the ARA_API project")

include("${ARA_API_DIR}/ARA_API_Helpers.cmake")
include("${ARA_API_DIR}/ARA_Version.cmake")

#set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE STRING "Generate compile commands" FORCE)

# ======================

project(ARA_Library
    DESCRIPTION "ARA Audio Random Access: Implementation Library"
    #only added in CMake 3.12:
    #HOMEPAGE_URL https://www.celemony.com/ara
    VERSION ${ARA_MAJOR_VERSION}.${ARA_MINOR_VERSION}.${ARA_PATCH_VERSION}.${ARA_BUILD_VERSION}
    LANGUAGES C CXX
)

# ======================

add_subdirectory("${ARA_API_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/ARA_API.build" EXCLUDE_FROM_ALL)
set_target_properties(ARA_API PROPERTIES
    XCODE_GENERATE_SCHEME OFF
)

# ======================

function(configure_ARA_Library_target target)

    # file grouping
    ara_group_target_files(${target})

    # language standards
    target_compile_features(${target}
        PUBLIC
            cxx_std_11
            c_std_11
    )

    # ARA_API dependency
    ara_link_ARA_API(${target} PUBLIC)

    # include paths
    target_include_directories(${target}
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/.."
    )

    # defines
    # \todo: add proper external configuration of the ARADebug options!
    target_compile_definitions(${target}
        # \todo is PRIVATE really appropriate here, or should it rather be PUBLIC?
        PRIVATE
            $<$<NOT:$<CONFIG:Debug>>: -DNDEBUG>
    )

    # platform-specific settings
    if(WIN32)
        target_compile_definitions(${target}
            PRIVATE
                -DNOMINMAX=1
        )
    elseif(APPLE)
        if(XCODE)
            set_target_properties(${target} PROPERTIES
                XCODE_ATTRIBUTE_GCC_SYMBOLS_PRIVATE_EXTERN YES
                XCODE_ATTRIBUTE_GCC_INLINES_ARE_PRIVATE_EXTERN YES
            )
        else()
            set_target_properties(${target} PROPERTIES
                C_VISIBILITY_PRESET hidden
                CXX_VISIBILITY_PRESET hidden
                VISIBILITY_INLINES_HIDDEN ON
            )
        endif()
    elseif(UNIX AND NOT APPLE)
        set_target_properties(${target} PROPERTIES
            POSITION_INDEPENDENT_CODE ON
        )
    endif()

    # default settings
    ara_disable_unwanted_warnings(${target})

endfunction()

# ======================

# files used in both host and plug-in target
set(ARA_Library_Common_Files
    "${CMAKE_CURRENT_SOURCE_DIR}/Debug/ARADebug.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Debug/ARADebug.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Debug/ARAContentLogger.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Debug/ARAContentValidator.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Dispatch/ARADispatchBase.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Dispatch/ARAContentReader.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utilities/ARAChannelArrangement.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utilities/ARAChannelArrangement.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utilities/ARAStdVectorUtilities.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utilities/ARASamplePositionConversion.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utilities/ARATimelineConversion.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utilities/ARAPitchInterpretation.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utilities/ARAPitchInterpretation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ARA_Library.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/ChangeLog.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt"
)

# ======================

# test if we can use object targets or must fall back to or static targets
# older CMake can't link to object targets, must build static lib instead
# \todo when building universal binaries for macOS, the paths to the object files are broken...
#       to work around, we need to stick with static libs for now.
#       CMake 3.20 promises a fix for the issue...
#if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.12)
#    set(ARA_LIBRARY_TARGET_TYPE OBJECT)
#else()
    set(ARA_LIBRARY_TARGET_TYPE STATIC)
#endif()

# ======================

add_library(ARA_Host_Library ${ARA_LIBRARY_TARGET_TYPE}
    ${ARA_Library_Common_Files}
    "${CMAKE_CURRENT_SOURCE_DIR}/Dispatch/ARAHostDispatch.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Dispatch/ARAHostDispatch.cpp"
)
configure_ARA_Library_target(ARA_Host_Library)

# ======================

add_library(ARA_PlugIn_Library ${ARA_LIBRARY_TARGET_TYPE}
    ${ARA_Library_Common_Files}
    "${CMAKE_CURRENT_SOURCE_DIR}/Dispatch/ARAPlugInDispatch.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Dispatch/ARAPlugInDispatch.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/PlugIn/ARAPlug.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/PlugIn/ARAPlug.cpp"
)

configure_ARA_Library_target(ARA_PlugIn_Library)

# ======================

add_library(ARA_IPC_Library ${ARA_LIBRARY_TARGET_TYPE}
    "${CMAKE_CURRENT_SOURCE_DIR}/IPC/ARAIPC.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/IPC/ARAIPCEncoding.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/IPC/ARAIPCProxyHost.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/IPC/ARAIPCProxyHost.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/IPC/ARAIPCProxyPlugIn.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/IPC/ARAIPCProxyPlugIn.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/IPC/ARAIPCProxyHost.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/IPC/ARAIPCProxyHost.cpp"
)

target_link_libraries(ARA_IPC_Library PRIVATE
    ARA_Host_Library
    ARA_PlugIn_Library
)

configure_ARA_Library_target(ARA_IPC_Library)
