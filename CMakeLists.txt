cmake_minimum_required(VERSION 3.20)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(_default_rippled "${CMAKE_CURRENT_SOURCE_DIR}/../rippled")
    set(_conan "${_default_rippled}/.build/build/generators/conan_toolchain.cmake")
    if(EXISTS "${_conan}")
        set(CMAKE_TOOLCHAIN_FILE "${_conan}" CACHE FILEPATH "Conan toolchain from rippled")
    endif()
endif()

project(edgy VERSION 0.2.1 LANGUAGES CXX)

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
    "Path to the rippled source tree")
set(RIPPLED_BUILD
    "${RIPPLED_ROOT}/.build"
    CACHE PATH
    "Path to the rippled CMake build that produced libxrpl")
set(XAHAUD_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../xahaud"
    CACHE PATH
    "Path to the xahaud source tree")
set(XAHAUD_BUILD
    "${XAHAUD_ROOT}/.build"
    CACHE PATH
    "Path to the xahaud CMake build that produced libxrpl")

if(NOT EXISTS "${RIPPLED_ROOT}/include/xrpl/tx/paths/RippleCalc.h")
    message(FATAL_ERROR "RIPPLED_ROOT does not look like rippled: ${RIPPLED_ROOT}")
endif()
if(NOT EXISTS "${RIPPLED_BUILD}/libxrpl.a")
    message(FATAL_ERROR "libxrpl.a not found in RIPPLED_BUILD=${RIPPLED_BUILD}. Build rippled first.")
endif()
if(NOT EXISTS "${XAHAUD_ROOT}/src/xrpld/app/paths/RippleCalc.cpp")
    message(FATAL_ERROR "XAHAUD_ROOT does not look like xahaud: ${XAHAUD_ROOT}")
endif()
if(NOT EXISTS "${XAHAUD_BUILD}/libxrpl.a")
    message(FATAL_ERROR "libxrpl.a not found in XAHAUD_BUILD=${XAHAUD_BUILD}. Build xahaud first.")
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

# Boost.Stacktrace from_exception redefines __cxa_allocate_exception.
# That is fine with a shared libstdc++; it is a multiple-definition (or a
# throw-time segfault) with -static-libstdc++. Drop it from the umbrella.
foreach(_st IN ITEMS boost::stacktrace_from_exception Boost::stacktrace_from_exception)
    if(TARGET ${_st})
        set_property(TARGET ${_st} PROPERTY INTERFACE_LINK_LIBRARIES "")
        foreach(_cfg IN ITEMS "" _RELEASE _DEBUG _RELWITHDEBINFO _MINSIZEREL)
            set_property(TARGET ${_st} PROPERTY IMPORTED_LOCATION${_cfg} "")
        endforeach()
    endif()
endforeach()
foreach(_umbrella IN ITEMS boost::boost Boost::boost)
    if(TARGET ${_umbrella})
        get_target_property(_boost_libs ${_umbrella} INTERFACE_LINK_LIBRARIES)
        if(_boost_libs)
            list(FILTER _boost_libs EXCLUDE REGEX "stacktrace_from_exception")
            set_property(TARGET ${_umbrella} PROPERTY INTERFACE_LINK_LIBRARIES "${_boost_libs}")
        endif()
    endif()
endforeach()
if(Boost_LIBRARIES)
    list(FILTER Boost_LIBRARIES EXCLUDE REGEX "stacktrace_from_exception")
endif()

set(EDGY_COMMON_SOURCES
    src/edgy/config.cpp
    src/edgy/memory_ledger.cpp
    src/edgy/protocol.cpp
    src/edgy/graph.cpp
    src/edgy/order_books.cpp
    src/edgy/node_client.cpp
    src/edgy/session.cpp
    src/edgy/engine.cpp
    src/edgy/server.cpp
    src/edgy/version.cpp
    src/vendor/path/AssetCache.cpp
    src/vendor/path/TrustLine.cpp
    src/vendor/path/AccountAssets.cpp
)

set(EDGY_XAHAUD_FLOW_SOURCES
    ${XAHAUD_ROOT}/src/xrpld/app/misc/detail/AMMHelpers.cpp
    ${XAHAUD_ROOT}/src/xrpld/app/misc/detail/AMMUtils.cpp
    ${XAHAUD_ROOT}/src/xrpld/app/paths/Credit.cpp
    ${XAHAUD_ROOT}/src/xrpld/app/paths/Flow.cpp
    ${XAHAUD_ROOT}/src/xrpld/app/paths/RippleCalc.cpp
    ${XAHAUD_ROOT}/src/xrpld/app/paths/detail/AMMLiquidity.cpp
    ${XAHAUD_ROOT}/src/xrpld/app/paths/detail/AMMOffer.cpp
    ${XAHAUD_ROOT}/src/xrpld/app/paths/detail/BookStep.cpp
    ${XAHAUD_ROOT}/src/xrpld/app/paths/detail/DirectStep.cpp
    ${XAHAUD_ROOT}/src/xrpld/app/paths/detail/PaySteps.cpp
    ${XAHAUD_ROOT}/src/xrpld/app/paths/detail/XRPEndpointStep.cpp
    ${XAHAUD_ROOT}/src/xrpld/app/tx/detail/BookTip.cpp
    ${XAHAUD_ROOT}/src/xrpld/app/tx/detail/OfferStream.cpp
    ${XAHAUD_ROOT}/src/xrpld/ledger/detail/ApplyStateTable.cpp
    ${XAHAUD_ROOT}/src/xrpld/ledger/detail/ApplyView.cpp
    ${XAHAUD_ROOT}/src/xrpld/ledger/detail/ApplyViewBase.cpp
    ${XAHAUD_ROOT}/src/xrpld/ledger/detail/OpenView.cpp
    ${XAHAUD_ROOT}/src/xrpld/ledger/detail/PaymentSandbox.cpp
    ${XAHAUD_ROOT}/src/xrpld/ledger/detail/RawStateTable.cpp
    ${XAHAUD_ROOT}/src/xrpld/ledger/detail/ReadView.cpp
    ${XAHAUD_ROOT}/src/xrpld/ledger/detail/View.cpp
)

add_library(xrpld_libxrpl STATIC IMPORTED)
set_target_properties(
    xrpld_libxrpl
    PROPERTIES IMPORTED_LOCATION "${RIPPLED_BUILD}/libxrpl.a"
)

add_library(xahaud_libxrpl STATIC IMPORTED)
set_target_properties(
    xahaud_libxrpl
    PROPERTIES IMPORTED_LOCATION "${XAHAUD_BUILD}/libxrpl.a"
)

function(edgy_link_common target)
    target_link_libraries(
        ${target}
        PUBLIC
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
        target_link_libraries(${target} PUBLIC "-framework CoreFoundation")
    endif()
    target_compile_definitions(
        ${target}
        PUBLIC
            BOOST_ASIO_DISABLE_HANDLER_TYPE_REQUIREMENTS
            BOOST_ASIO_USE_TS_EXECUTOR_AS_DEFAULT
            BOOST_BEAST_ALLOW_DEPRECATED
            BOOST_CONTAINER_FWD_BAD_DEQUE
            EDGY_VERSION="${PROJECT_VERSION}"
    )
    if(EDGY_GIT_HASH)
        target_compile_definitions(${target} PUBLIC EDGY_GIT_HASH="${EDGY_GIT_HASH}")
    endif()
endfunction()

add_library(edgy_xrpld_core STATIC
    ${EDGY_COMMON_SOURCES}
    src/vendor/path/Pathfinder.cpp
)
target_include_directories(
    edgy_xrpld_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${RIPPLED_ROOT}/include
        ${RIPPLED_BUILD}/pb-xrpl.libpb
)
file(GLOB _xrpld_module_dirs "${RIPPLED_BUILD}/modules/xrpl.libxrpl.*")
foreach(_dir IN LISTS _xrpld_module_dirs)
    target_include_directories(edgy_xrpld_core PUBLIC ${_dir})
endforeach()
target_link_libraries(edgy_xrpld_core PUBLIC xrpld_libxrpl)
edgy_link_common(edgy_xrpld_core)

add_library(edgy_xahaud_core STATIC
    ${EDGY_COMMON_SOURCES}
    ${EDGY_XAHAUD_FLOW_SOURCES}
)
target_compile_definitions(
    edgy_xahaud_core
    PUBLIC
        EDGY_XAHAU
        xrpl=ripple
)
target_include_directories(
    edgy_xahaud_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include/edgy/xahau_compat
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${XAHAUD_ROOT}/include
        ${XAHAUD_ROOT}/src
        ${XAHAUD_BUILD}/pb-xrpl.libpb
)
file(GLOB _xahaud_module_dirs "${XAHAUD_BUILD}/modules/xrpl.libxrpl.*")
foreach(_dir IN LISTS _xahaud_module_dirs)
    target_include_directories(edgy_xahaud_core PUBLIC ${_dir})
endforeach()
target_link_libraries(edgy_xahaud_core PUBLIC xahaud_libxrpl)
edgy_link_common(edgy_xahaud_core)

add_executable(edgy-xrpld src/app/main.cpp)
target_link_libraries(edgy-xrpld PRIVATE edgy_xrpld_core)

add_executable(edgy-xahaud src/app/main.cpp)
target_link_libraries(edgy-xahaud PRIVATE edgy_xahaud_core)

enable_testing()

function(edgy_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE edgy_xrpld_core)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

edgy_add_test(edgy_test_reprice tests/reprice_tests.cpp)
edgy_add_test(edgy_test_config tests/config_tests.cpp)
edgy_add_test(edgy_test_path tests/edgy_tests.cpp)

# Legacy name so `.build/edgy_tests` still works.
add_executable(edgy_tests tests/edgy_tests.cpp)
target_link_libraries(edgy_tests PRIVATE edgy_xrpld_core)

include(GNUInstallDirs)
install(TARGETS edgy-xrpld edgy-xahaud RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(
    FILES
        ${CMAKE_SOURCE_DIR}/cfg/edgy-xrpl.example.cfg
        ${CMAKE_SOURCE_DIR}/cfg/edgy-xahau.example.cfg
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/edgy
)
