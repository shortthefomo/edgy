cmake_minimum_required(VERSION 3.20)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(_default_rippled "${CMAKE_CURRENT_SOURCE_DIR}/../rippled")
    set(_conan "${_default_rippled}/.build/build/generators/conan_toolchain.cmake")
    if(EXISTS "${_conan}")
        set(CMAKE_TOOLCHAIN_FILE "${_conan}" CACHE FILEPATH "Conan toolchain from rippled")
    endif()
endif()

project(edgy VERSION 0.1.0 LANGUAGES CXX)

set(EDGY_GIT_HASH "")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=8 HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE EDGY_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

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

add_library(edgy_core STATIC
    src/edgy/config.cpp
    src/edgy/memory_ledger.cpp
    src/edgy/graph.cpp
    src/edgy/order_books.cpp
    src/edgy/node_client.cpp
    src/edgy/session.cpp
    src/edgy/engine.cpp
    src/edgy/server.cpp
    src/edgy/version.cpp
    src/vendor/path/Pathfinder.cpp
    src/vendor/path/AssetCache.cpp
    src/vendor/path/TrustLine.cpp
    src/vendor/path/AccountAssets.cpp
)

target_include_directories(
    edgy_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${RIPPLED_ROOT}/include
        ${RIPPLED_BUILD}/pb-xrpl.libpb
)

file(GLOB _xrpl_module_dirs "${RIPPLED_BUILD}/modules/xrpl.libxrpl.*")
foreach(_dir IN LISTS _xrpl_module_dirs)
    target_include_directories(edgy_core PUBLIC ${_dir})
endforeach()

target_compile_definitions(
    edgy_core
    PUBLIC
        BOOST_ASIO_DISABLE_HANDLER_TYPE_REQUIREMENTS
        BOOST_ASIO_USE_TS_EXECUTOR_AS_DEFAULT
        BOOST_BEAST_ALLOW_DEPRECATED
        BOOST_CONTAINER_FWD_BAD_DEQUE
        EDGY_VERSION="${PROJECT_VERSION}"
)
if(EDGY_GIT_HASH)
    target_compile_definitions(edgy_core PUBLIC EDGY_GIT_HASH="${EDGY_GIT_HASH}")
endif()

target_link_libraries(
    edgy_core
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
    target_link_libraries(edgy_core PUBLIC "-framework CoreFoundation")
endif()

add_executable(edgy src/app/main.cpp)
target_link_libraries(edgy PRIVATE edgy_core)

add_executable(edgy_tests tests/edgy_tests.cpp)
target_link_libraries(edgy_tests PRIVATE edgy_core)

enable_testing()
add_test(NAME edgy_tests COMMAND edgy_tests)

include(GNUInstallDirs)
install(TARGETS edgy RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(
    FILES ${CMAKE_SOURCE_DIR}/cfg/edgy.example.cfg
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/edgy
    RENAME edgy.cfg
)
