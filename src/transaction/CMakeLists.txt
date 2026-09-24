target_sources(miniDB PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/transaction_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/heap_file.cpp
)

target_include_directories(miniDB PUBLIC ${CMAKE_SOURCE_DIR}/include/transaction)