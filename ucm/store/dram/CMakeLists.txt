add_library(dramstore SHARED cc/dram_store.cc)
target_include_directories(dramstore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cc)
target_include_directories(dramstore PUBLIC
    ${CMAKE_SOURCE_DIR}/ucm/transport/p2p/include/one_sided)
target_link_libraries(dramstore PUBLIC storeintf infra_logger infra_thread buffer_pool)
file(RELATIVE_PATH INSTALL_REL_PATH ${UCM_ROOT_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS dramstore LIBRARY DESTINATION ${INSTALL_REL_PATH} COMPONENT ucm)
