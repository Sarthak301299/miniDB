add_library(miniDB SHARED
    disk_manager.cpp
    wal_manager.cpp
    buffer_pool.cpp
    recovery.cpp
)

target_include_directories(miniDB PUBLIC ${CMAKE_SOURCE_DIR}/include/storage)