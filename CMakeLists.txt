cmake_minimum_required(VERSION 3.13)

if (NOT DEFINED ENV{DEVKITPRO})
    message(FATAL_ERROR "DEVKITPRO is not defined!")
endif()

if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if (EXISTS $ENV{DEVKITPRO}/cmake/Switch.cmake)
        set(CMAKE_TOOLCHAIN_FILE $ENV{DEVKITPRO}/cmake/Switch.cmake)
    else()
        message(FATAL_ERROR "please run 'sudo pacman -S switch-cmake`")
    endif()
endif()

project(sys-ftpsrv
    VERSION 1.0.0
    LANGUAGES C CXX
)

# the atmosphere program id the sysmodule is installed under. kept distinct from
# sys-ftpd (420000000000000E) and upstream ftpsrv's sys-ftp (420000000000011B)
# so that they can be installed side by side (only enable one at a time).
set(SYS_FTPSRV_PROGRAM_ID "4200000000000F7B" CACHE STRING "sysmodule program id (16 hex digits, no 0x)")

# enable setting cmake options via set()
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if (ipo_supported)
        cmake_policy(SET CMP0069 NEW)
        set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# same ftpsrv revision that sphaira ships.
FetchContent_Declare(ftpsrv
    GIT_REPOSITORY https://github.com/ITotalJustice/ftpsrv.git
    GIT_TAG 7c82402
    SOURCE_SUBDIR NONE
)

FetchContent_Declare(minIni
    GIT_REPOSITORY https://github.com/ITotalJustice/minIni-nx.git
    GIT_TAG 11cac8b
)

FetchContent_Declare(libultrahand
    GIT_REPOSITORY https://github.com/ppkantorski/libultrahand.git
    GIT_TAG v2.5.3
    SOURCE_SUBDIR NONE
)

set(MININI_USE_NX ON)
set(MININI_USE_STDIO OFF)
set(MININI_USE_FLOAT OFF)

FetchContent_MakeAvailable(ftpsrv minIni libultrahand)

# release layout, copy the contents of this folder to the root of the sd card.
set(SD_OUT ${CMAKE_BINARY_DIR}/out)
set(SYSMOD_OUT ${SD_OUT}/atmosphere/contents/${SYS_FTPSRV_PROGRAM_ID})

# ---------------------------------------------------------------------------
# shared config (used by both the sysmodule and overlay)
# ---------------------------------------------------------------------------
# config handling uses cfgini (MIT) rather than minIni (Apache-2.0), so that
# no Apache code ends up in the GPL overlay.
add_library(sys_ftpsrv_config STATIC
    common/sys_ftpsrv_config.c
    common/cfgini.c
)
target_include_directories(sys_ftpsrv_config PUBLIC common)
target_compile_definitions(sys_ftpsrv_config PUBLIC
    SYS_FTPSRV_PROGRAM_ID=0x${SYS_FTPSRV_PROGRAM_ID}ULL
    SYS_FTPSRV_PROGRAM_ID_STR="${SYS_FTPSRV_PROGRAM_ID}"
    SYS_FTPSRV_VERSION="${PROJECT_VERSION}"
)

# ---------------------------------------------------------------------------
# sysmodule
# ---------------------------------------------------------------------------
set(FTPSRV_LIB_BUILD TRUE)
set(FTPSRV_LIB_VFS_CUSTOM ${ftpsrv_SOURCE_DIR}/src/platform/nx/vfs_nx.h)
set(FTPSRV_LIB_PATH_SIZE 0x301)
# matches upstream's sysmodule, every session reserves a 64k file buffer.
set(FTPSRV_LIB_SESSIONS 5)
set(FTPSRV_LIB_BUF_SIZE 1024*64)
set(FTPSRV_LIB_CUSTOM_DEFINES
    USE_VFS_SAVE=1
    USE_VFS_STORAGE=1
    USE_VFS_GC=1
    USE_VFS_USBHSFS=0
    USE_VFS_ROMFS=0
    # keeps memory usage down, same as upstream's sysmodule.
    VFS_NX_BUFFER_IO=0
    FTP_SOCKET_HEADER="${ftpsrv_SOURCE_DIR}/src/platform/nx/socket_nx.h"
)

add_subdirectory(${ftpsrv_SOURCE_DIR} ftpsrv_build)

set(FTPSRV_NX ${ftpsrv_SOURCE_DIR}/src/platform/nx)

add_executable(sys-ftpsrv
    sysmodule/source/main.c
    ${FTPSRV_NX}/vfs_nx.c
    ${FTPSRV_NX}/vfs/vfs_nx_none.c
    ${FTPSRV_NX}/vfs/vfs_nx_root.c
    ${FTPSRV_NX}/vfs/vfs_nx_fs.c
    ${FTPSRV_NX}/vfs/vfs_nx_save.c
    ${FTPSRV_NX}/vfs/vfs_nx_storage.c
    ${FTPSRV_NX}/vfs/vfs_nx_gc.c
    ${FTPSRV_NX}/reboot_to_payload/ams_bpc.c
    ${FTPSRV_NX}/reboot_to_payload/reboot_to_payload.c
    ${FTPSRV_NX}/rtc/max77620-rtc.c
    ${FTPSRV_NX}/custom_commands.c
    ${FTPSRV_NX}/utils.c
    ${ftpsrv_SOURCE_DIR}/src/log/log.c
)

set_target_properties(sys-ftpsrv PROPERTIES
    C_STANDARD 11
    C_EXTENSIONS ON
)

target_include_directories(sys-ftpsrv PRIVATE
    ${FTPSRV_NX}
    ${ftpsrv_SOURCE_DIR}/src
)

# minIni is only needed by ftpsrv's custom_commands.c (hekate ini parsing).
target_link_libraries(sys-ftpsrv PRIVATE ftpsrv sys_ftpsrv_config minIni)

configure_file(sysmodule/sys-ftpsrv.json.in ${CMAKE_BINARY_DIR}/sys-ftpsrv.json @ONLY)
configure_file(sysmodule/toolbox.json.in ${SYSMOD_OUT}/toolbox.json @ONLY)
file(MAKE_DIRECTORY ${SYSMOD_OUT}/flags)
file(TOUCH ${SYSMOD_OUT}/flags/boot2.flag)
configure_file(assets/config.ini.template ${SD_OUT}/config/sys-ftpsrv/config.ini.template COPYONLY)

# license texts travel with the binaries in the release zip.
file(GLOB THIRD_PARTY_LICENSE_TEXTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/licenses/*)
foreach(license ${THIRD_PARTY_LICENSE_TEXTS} LICENSE LICENSE-MIT THIRD_PARTY_LICENSES.md)
    get_filename_component(license_name ${license} NAME)
    configure_file(${license} ${SD_OUT}/config/sys-ftpsrv/licenses/${license_name} COPYONLY)
endforeach()

nx_create_exefs(sys-ftpsrv
    OUTPUT ${SYSMOD_OUT}/exefs.nsp
    CONFIG ${CMAKE_BINARY_DIR}/sys-ftpsrv.json
)

# ---------------------------------------------------------------------------
# overlay (libultrahand)
# ---------------------------------------------------------------------------
file(GLOB LIBULTRA_SOURCES CONFIGURE_DEPENDS ${libultrahand_SOURCE_DIR}/libultra/source/*.cpp)

add_executable(ovl-sys-ftpsrv
    overlay/source/main.cpp
    ${libultrahand_SOURCE_DIR}/common/cJSON.c
    ${libultrahand_SOURCE_DIR}/libtesla/source/tesla.cpp
    ${LIBULTRA_SOURCES}
)

target_include_directories(ovl-sys-ftpsrv PRIVATE
    ${libultrahand_SOURCE_DIR}/common
    ${libultrahand_SOURCE_DIR}/libultra/include
    ${libultrahand_SOURCE_DIR}/libtesla/include
)

target_compile_definitions(ovl-sys-ftpsrv PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    # theme / wallpaper / translations can be overridden from here.
    UI_OVERRIDE_PATH="/config/sys-ftpsrv/"
    USE_EXCEPTION_WRAP=1
)

target_compile_options(ovl-sys-ftpsrv PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++26>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-unwind-tables>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-asynchronous-unwind-tables>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-dangling-else>
    -ffunction-sections
    -fdata-sections
)

target_link_options(ovl-sys-ftpsrv PRIVATE
    -Wl,--gc-sections
    -Wl,-wrap,__cxa_throw
    -Wl,-wrap,_Unwind_Resume
    -Wl,-wrap,__gxx_personality_v0
)

# libultra's download helpers need curl, they are stripped by gc-sections
# as the overlay never calls them.
find_library(CURL_LIB curl REQUIRED)
find_library(MINIZIP_LIB minizip REQUIRED)
find_library(MBEDTLS_LIB mbedtls REQUIRED)
find_library(MBEDX509_LIB mbedx509 REQUIRED)
find_library(MBEDCRYPTO_LIB mbedcrypto REQUIRED)
find_library(Z_LIB z REQUIRED)
find_path(CURL_INCLUDE_DIR curl/curl.h REQUIRED)

target_include_directories(ovl-sys-ftpsrv PRIVATE ${CURL_INCLUDE_DIR})

target_link_libraries(ovl-sys-ftpsrv PRIVATE
    sys_ftpsrv_config
    ${CURL_LIB}
    ${MINIZIP_LIB}
    ${MBEDTLS_LIB}
    ${MBEDX509_LIB}
    ${MBEDCRYPTO_LIB}
    ${Z_LIB}
)

nx_generate_nacp(
    OUTPUT ovl-sys-ftpsrv.nacp
    NAME "sys-ftpsrv"
    AUTHOR "sphaira contributors"
    VERSION ${PROJECT_VERSION}
)

set(OVL_OUT ${SD_OUT}/switch/.overlays/sys-ftpsrv.ovl)

# built by hand rather than with nx_create_nro so that the ultrahand
# signature is appended exactly once per link.
add_custom_command(
    OUTPUT ${OVL_OUT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SD_OUT}/switch/.overlays
    COMMAND ${NX_ELF2NRO_EXE} $<TARGET_FILE:ovl-sys-ftpsrv> ${OVL_OUT} --nacp=${CMAKE_BINARY_DIR}/ovl-sys-ftpsrv.nacp
    COMMAND ${CMAKE_COMMAND} -DFILE=${OVL_OUT} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/append_ultr_signature.cmake
    DEPENDS ovl-sys-ftpsrv ${CMAKE_BINARY_DIR}/ovl-sys-ftpsrv.nacp
    COMMENT "Building Ultrahand overlay sys-ftpsrv.ovl"
    VERBATIM
)

add_custom_target(ovl-sys-ftpsrv_ovl ALL DEPENDS ${OVL_OUT})
