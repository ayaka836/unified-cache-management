file(GLOB_RECURSE UCM_REMOTEMEM_STORE_CC_SOURCE_FILES "./cc/*.cc")
add_library(remotememstore SHARED ${UCM_REMOTEMEM_STORE_CC_SOURCE_FILES})
target_include_directories(remotememstore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cc)
target_link_libraries(remotememstore PUBLIC storeintf trans infra_logger)

file(RELATIVE_PATH INSTALL_REL_PATH ${UCM_ROOT_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS remotememstore LIBRARY DESTINATION ${INSTALL_REL_PATH} COMPONENT ucm)
