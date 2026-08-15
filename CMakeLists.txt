cmake_minimum_required(VERSION 3.20)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(_default_rippled "${CMAKE_CURRENT_SOURCE_DIR}/../rippled")
    set(_conan "${_default_rippled}/.build/build/generators/conan_toolchain.cmake")
    if(EXISTS "${_conan}")
        set(CMAKE_TOOLCHAIN_FILE "${_conan}" CACHE FILEPATH "Conan toolchain from rippled")
    endif()
endif()

project(pathfinder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(RIPPLED_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../rippled"
    CACHE PATH
    "Path to the rippled source tree (reference implementation)")
set(RIPPLED_BUILD
    "${RIPPLED_ROOT}/.build"
    CACHE PATH
    "Path to the rippled CMake build that produced libxrpl")

if(NOT EXISTS "${RIPPLED_ROOT}/include/xrpl/tx/paths/RippleCalc.h")
    message(FATAL_ERROR "RIPPLED_ROOT does not look like rippled: ${RIPPLED_ROOT}")
endif()
if(NOT EXISTS "${RIPPLED_BUILD}/libxrpl.a")
    message(
        FATAL_ERROR
        "libxrpl.a not found in RIPPLED_BUILD=${RIPPLED_BUILD}. Build rippled first."
    )
endif()

find_package(Boost REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(secp256k1 REQUIRED)
find_package(ed25519 REQUIRED)
find_package(xxHash REQUIRED)
find_package(date REQUIRED)
find_package(FastFloat REQUIRED)
find_package(mpt-crypto REQUIRED)
find_package(lz4 REQUIRED)
find_package(LibArchive REQUIRED)

add_library(xrpl_lib STATIC IMPORTED)
set_target_properties(
    xrpl_lib
    PROPERTIES IMPORTED_LOCATION "${RIPPLED_BUILD}/libxrpl.a"
)

add_library(pathfinder_core STATIC
    src/pathfinder/config.cpp
    src/pathfinder/memory_ledger.cpp
    src/pathfinder/graph.cpp
    src/pathfinder/order_books.cpp
    src/pathfinder/node_client.cpp
    src/pathfinder/session.cpp
    src/pathfinder/engine.cpp
    src/pathfinder/server.cpp
    src/vendor/path/Pathfinder.cpp
    src/vendor/path/AssetCache.cpp
    src/vendor/path/TrustLine.cpp
    src/vendor/path/AccountAssets.cpp
)

target_include_directories(
    pathfinder_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${RIPPLED_ROOT}/include
        ${RIPPLED_BUILD}/pb-xrpl.libpb
)

file(GLOB _xrpl_module_dirs "${RIPPLED_BUILD}/modules/xrpl.libxrpl.*")
foreach(_dir IN LISTS _xrpl_module_dirs)
    target_include_directories(pathfinder_core PUBLIC ${_dir})
endforeach()

target_compile_definitions(
    pathfinder_core
    PUBLIC
        BOOST_ASIO_DISABLE_HANDLER_TYPE_REQUIREMENTS
        BOOST_ASIO_USE_TS_EXECUTOR_AS_DEFAULT
        BOOST_BEAST_ALLOW_DEPRECATED
        BOOST_CONTAINER_FWD_BAD_DEQUE
)

target_link_libraries(
    pathfinder_core
    PUBLIC
        xrpl_lib
        boost::boost
        ${Boost_LIBRARIES}
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
        secp256k1::secp256k1
        ed25519::ed25519
        xxHash::xxhash
        date::date
        FastFloat::fast_float
        mpt-crypto::mpt-crypto
        lz4::lz4
        LibArchive::LibArchive
)

if(APPLE)
    target_link_libraries(pathfinder_core PUBLIC "-framework CoreFoundation")
endif()

add_executable(pathfinder src/app/main.cpp)
target_link_libraries(pathfinder PRIVATE pathfinder_core)

add_executable(pathfinder_tests tests/pathfinder_tests.cpp)
target_link_libraries(pathfinder_tests PRIVATE pathfinder_core)

enable_testing()
add_test(NAME pathfinder_tests COMMAND pathfinder_tests)
