target_sources(miniDB PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/disk_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wal_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/buffer_pool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/recovery.cpp
)

target_include_directories(miniDB PUBLIC ${CMAKE_SOURCE_DIR}/include/storage)