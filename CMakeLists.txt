cmake_minimum_required(VERSION 3.16)

project(YuanBook VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(YuanBook
    yuanbook/YuanBook.cpp
    yuanbook/AuthSecurity.cpp
    yuanbook/WebServer.cpp
    yuanbook/LedgerManager.cpp
    yuanbook/RuntimeSettings.cpp
    yuanbook/VoiceLedgerManager.cpp
    yuanbook/BillImportManager.cpp
    yuanbook/BillImportUtils.cpp
    yuanbook/JsonLite.cpp
    yuanbook/SystemUtils.cpp
    yuanbook/WorkLog.cpp
)

set(YUANBOOK_THIRD_PARTY_INCLUDE_DIR "" CACHE PATH "Directory containing optional third-party headers such as httplib.h and sqlite3.h")
set(YUANBOOK_SQLITE3_LIBRARY "" CACHE FILEPATH "Optional explicit sqlite3 library path")
if(WIN32)
    set(YUANBOOK_USE_BUNDLED_SQLITE_DEFAULT ON)
else()
    set(YUANBOOK_USE_BUNDLED_SQLITE_DEFAULT OFF)
endif()
option(YUANBOOK_USE_BUNDLED_SQLITE "Build with yuanbook/sqlite3.c instead of requiring a system SQLite library" ${YUANBOOK_USE_BUNDLED_SQLITE_DEFAULT})
set(YUANBOOK_BUNDLED_SQLITE_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/yuanbook/sqlite3.c)

target_include_directories(YuanBook PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/yuanbook
)

if(YUANBOOK_THIRD_PARTY_INCLUDE_DIR)
    target_include_directories(YuanBook PRIVATE ${YUANBOOK_THIRD_PARTY_INCLUDE_DIR})
endif()

find_path(HTTPLIB_INCLUDE_DIR httplib.h
    HINTS
        ${YUANBOOK_THIRD_PARTY_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/external
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor
        ${CMAKE_CURRENT_SOURCE_DIR}/yuanbook
)

if(HTTPLIB_INCLUDE_DIR)
    target_include_directories(YuanBook PRIVATE ${HTTPLIB_INCLUDE_DIR})
else()
    message(WARNING "httplib.h was not found. Download cpp-httplib single header to yuanbook/httplib.h or pass -DYUANBOOK_THIRD_PARTY_INCLUDE_DIR=<dir>.")
endif()

target_compile_definitions(YuanBook PRIVATE
    PROJECT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

find_package(Threads REQUIRED)
target_link_libraries(YuanBook PRIVATE Threads::Threads)

if(YUANBOOK_SQLITE3_LIBRARY)
    target_link_libraries(YuanBook PRIVATE ${YUANBOOK_SQLITE3_LIBRARY})
elseif(YUANBOOK_USE_BUNDLED_SQLITE AND EXISTS ${YUANBOOK_BUNDLED_SQLITE_SOURCE})
    message(STATUS "Using bundled SQLite source: ${YUANBOOK_BUNDLED_SQLITE_SOURCE}")
    target_sources(YuanBook PRIVATE ${YUANBOOK_BUNDLED_SQLITE_SOURCE})
else()
    find_package(SQLite3 QUIET)
    if(SQLite3_FOUND)
        target_link_libraries(YuanBook PRIVATE SQLite::SQLite3)
    elseif(EXISTS ${YUANBOOK_BUNDLED_SQLITE_SOURCE})
        message(STATUS "SQLite3 development package was not found; falling back to bundled SQLite source: ${YUANBOOK_BUNDLED_SQLITE_SOURCE}")
        target_sources(YuanBook PRIVATE ${YUANBOOK_BUNDLED_SQLITE_SOURCE})
    else()
        message(WARNING "SQLite3 development package was not found and yuanbook/sqlite3.c is missing. Install sqlite3 development files or pass -DYUANBOOK_SQLITE3_LIBRARY=<lib> and optionally -DYUANBOOK_THIRD_PARTY_INCLUDE_DIR=<dir>.")
        target_link_libraries(YuanBook PRIVATE sqlite3)
    endif()
    
endif()

if(WIN32)
    target_compile_definitions(YuanBook PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(YuanBook PRIVATE ws2_32)
else()
    target_compile_definitions(YuanBook PRIVATE _DEFAULT_SOURCE)
endif()
