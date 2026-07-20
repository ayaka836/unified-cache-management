if(NOT RUNTIME_ENVIRONMENT STREQUAL "ascend")
    message(FATAL_ERROR "BUILD_UCM_ASU requires RUNTIME_ENVIRONMENT=ascend")
endif()

if(NOT DEFINED ASCEND_ROOT)
    if(DEFINED ENV{ASCEND_HOME_PATH})
        set(ASCEND_ROOT "$ENV{ASCEND_HOME_PATH}" CACHE PATH "Path to Ascend root directory")
    elseif(DEFINED ENV{ASCEND_TOOLKIT_HOME})
        set(ASCEND_ROOT "$ENV{ASCEND_TOOLKIT_HOME}" CACHE PATH "Path to Ascend root directory")
    else()
        set(ASCEND_ROOT "/usr/local/Ascend/ascend-toolkit/latest" CACHE PATH
            "Path to Ascend root directory")
    endif()
endif()

if(NOT DEFINED HIXL_ROOT AND DEFINED ENV{HIXL_ROOT})
    set(HIXL_ROOT "$ENV{HIXL_ROOT}" CACHE PATH "HIXL installation prefix")
endif()
if(NOT DEFINED HIXL_INCLUDE_DIR AND DEFINED ENV{HIXL_INCLUDE_DIR})
    set(HIXL_INCLUDE_DIR "$ENV{HIXL_INCLUDE_DIR}" CACHE PATH "Directory containing hixl headers")
endif()
if(NOT DEFINED HIXL_LIB_DIR AND DEFINED ENV{HIXL_LIB_DIR})
    set(HIXL_LIB_DIR "$ENV{HIXL_LIB_DIR}" CACHE PATH "Directory containing the HIXL library")
endif()

set(P2P_ASCEND_ROOT_HINTS ${ASCEND_ROOT})
foreach(P2P_ASCEND_PARENT_HINT
        "${ASCEND_ROOT}" "${ASCEND_ROOT}/.." "${ASCEND_ROOT}/../..")
    file(GLOB P2P_CANN_ROOT_HINTS LIST_DIRECTORIES true
        "${P2P_ASCEND_PARENT_HINT}/cann-*"
    )
    list(APPEND P2P_ASCEND_ROOT_HINTS ${P2P_CANN_ROOT_HINTS})
endforeach()
list(REMOVE_DUPLICATES P2P_ASCEND_ROOT_HINTS)

set(P2P_ASCEND_INCLUDE_HINTS)
set(P2P_ASCEND_LIBRARY_HINTS)
foreach(P2P_ASCEND_ROOT_HINT IN LISTS P2P_ASCEND_ROOT_HINTS)
    list(APPEND P2P_ASCEND_INCLUDE_HINTS
        ${P2P_ASCEND_ROOT_HINT}/include
        ${P2P_ASCEND_ROOT_HINT}/aarch64-linux/include
        ${P2P_ASCEND_ROOT_HINT}/arm64-linux/include
    )
    list(APPEND P2P_ASCEND_LIBRARY_HINTS
        ${P2P_ASCEND_ROOT_HINT}/lib64
        ${P2P_ASCEND_ROOT_HINT}/aarch64-linux/lib64
        ${P2P_ASCEND_ROOT_HINT}/aarch64-linux/devlib
        ${P2P_ASCEND_ROOT_HINT}/arm64-linux/lib64
        ${P2P_ASCEND_ROOT_HINT}/arm64-linux/devlib
    )
endforeach()

find_path(P2P_ASCEND_INCLUDE_DIR NAMES acl/acl.h HINTS ${P2P_ASCEND_INCLUDE_HINTS})
find_library(P2P_ASCENDCL_LIBRARY NAMES ascendcl HINTS ${P2P_ASCEND_LIBRARY_HINTS})
if(NOT P2P_ASCEND_INCLUDE_DIR OR NOT P2P_ASCENDCL_LIBRARY)
    message(FATAL_ERROR "Cannot find ACL dependencies under ASCEND_ROOT=${ASCEND_ROOT}")
endif()

set(P2P_HIXL_INCLUDE_HINTS
    ${HIXL_INCLUDE_DIR}
    ${HIXL_ROOT}
    ${HIXL_ROOT}/include
    ${HIXL_ROOT}/aarch64-linux/include
    ${HIXL_ROOT}/arm64-linux/include
)
foreach(P2P_ASCEND_ROOT_HINT IN LISTS P2P_ASCEND_ROOT_HINTS)
    list(APPEND P2P_HIXL_INCLUDE_HINTS
        ${P2P_ASCEND_ROOT_HINT}
        ${P2P_ASCEND_ROOT_HINT}/include
        ${P2P_ASCEND_ROOT_HINT}/aarch64-linux/include
        ${P2P_ASCEND_ROOT_HINT}/arm64-linux/include
    )
endforeach()

find_path(P2P_HIXL_INCLUDE_DIR
    NAMES hixl/hixl.h hixl/hixl_types.h
    HINTS ${P2P_HIXL_INCLUDE_HINTS}
)
if(NOT P2P_HIXL_INCLUDE_DIR)
    message(FATAL_ERROR
        "Cannot find hixl/hixl.h. Set HIXL_ROOT or HIXL_INCLUDE_DIR.")
endif()

set(P2P_HIXL_LIBRARY_HINTS
    ${HIXL_LIB_DIR}
    ${HIXL_ROOT}
    ${HIXL_ROOT}/lib
    ${HIXL_ROOT}/lib64
    ${HIXL_ROOT}/aarch64-linux/lib64
    ${HIXL_ROOT}/arm64-linux/lib64
)
foreach(P2P_ASCEND_ROOT_HINT IN LISTS P2P_ASCEND_ROOT_HINTS)
    list(APPEND P2P_HIXL_LIBRARY_HINTS
        ${P2P_ASCEND_ROOT_HINT}
        ${P2P_ASCEND_ROOT_HINT}/lib
        ${P2P_ASCEND_ROOT_HINT}/lib64
        ${P2P_ASCEND_ROOT_HINT}/aarch64-linux/lib64
        ${P2P_ASCEND_ROOT_HINT}/arm64-linux/lib64
    )
endforeach()

find_library(P2P_HIXL_LIBRARY
    NAMES cann_hixl
    HINTS ${P2P_HIXL_LIBRARY_HINTS}
)
if(NOT P2P_HIXL_LIBRARY)
    message(FATAL_ERROR
        "Cannot find libcann_hixl. Set HIXL_ROOT or HIXL_LIB_DIR.")
endif()

find_library(P2P_METADEF_LIBRARY
    NAMES metadef
    HINTS ${P2P_HIXL_LIBRARY_HINTS}
)
if(NOT P2P_METADEF_LIBRARY)
    message(FATAL_ERROR
        "Cannot find libmetadef under HIXL_ROOT, HIXL_LIB_DIR, or ASCEND_ROOT.")
endif()

set(P2P_TRANSPORT_SOURCES
    src/one_sided/common/acl_runtime_context.cpp
    src/one_sided/common/metadata_codec.cpp
    src/one_sided/control/metadata_channel.cpp
    src/one_sided/core/transport_manager.cpp
    src/one_sided/hixl/hixl_instance.cpp
    src/one_sided/hixl/hixl_transport.cpp
    src/two_sided/tcp/tcp_message_channel.cpp
)

add_library(ucm_p2p_transport SHARED ${P2P_TRANSPORT_SOURCES})
target_include_directories(ucm_p2p_transport
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/one_sided>
        $<INSTALL_INTERFACE:ucm/transport/p2p/include>
        $<INSTALL_INTERFACE:ucm/transport/p2p/include/one_sided>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/one_sided
        ${UCM_ROOT_DIR}/ucm/shared/infra
        ${P2P_ASCEND_INCLUDE_DIR}
        ${P2P_HIXL_INCLUDE_DIR}
)
target_compile_definitions(ucm_p2p_transport PRIVATE SPDLOG_FMT_EXTERNAL)
target_link_libraries(ucm_p2p_transport
    PRIVATE
        ${P2P_ASCENDCL_LIBRARY}
        infra_logger
        ${P2P_HIXL_LIBRARY}
        ${P2P_METADEF_LIBRARY}
        pthread
        rt
)
set_target_properties(ucm_p2p_transport PROPERTIES
    INSTALL_RPATH "$ORIGIN"
)

file(RELATIVE_PATH P2P_INSTALL_REL_PATH ${UCM_ROOT_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS ucm_p2p_transport
    LIBRARY DESTINATION ${P2P_INSTALL_REL_PATH}
    COMPONENT ucm
)
install(DIRECTORY include/
    DESTINATION ${P2P_INSTALL_REL_PATH}/include
    COMPONENT ucm
    FILES_MATCHING PATTERN "*.h"
)
