set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/$<CONFIG>)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR}/CMakeModules)
set(AZAHAR_LIBRETRO_OUTPUT_NAME "azahar_libretro" CACHE STRING
    "Output name for the Azahar libretro shared library")
option(AZAHAR_LIBRETRO_DEVELOPMENT "Build the isolated development core variant" OFF)

# Object library for libretro code (can be linked into both shared lib and tests)
add_library(citra_libretro_common OBJECT
        emu_window/libretro_window.cpp
        emu_window/libretro_window.h
        input/input_factory.cpp
        input/input_factory.h
        input/mouse_tracker.cpp
        input/mouse_tracker.h
        $<$<BOOL:${ENABLE_VULKAN}>: libretro_vk.cpp libretro_vk.h>
        environment.cpp
        environment.h
        core_settings.cpp
        core_settings.h
        tamariba_rooms.cpp
        tamariba_rooms.h)

target_compile_definitions(citra_libretro_common PRIVATE HAVE_LIBRETRO)
if(AZAHAR_LIBRETRO_DEVELOPMENT)
    target_compile_definitions(citra_libretro_common PRIVATE AZAHAR_LIBRETRO_DEVELOPMENT=1)
    if(ANDROID)
        # The frontend may say where its FFmpeg is; by default it sits next to this tree.
        if(NOT GBASHIELD_FFMPEG_ROOT)
            set(GBASHIELD_FFMPEG_ROOT "${PROJECT_SOURCE_DIR}/../ffmpeg-android-arm64")
        endif()
        foreach(FFMPEG_LIBRARY avcodec avutil swscale)
            if(NOT EXISTS "${GBASHIELD_FFMPEG_ROOT}/lib/lib${FFMPEG_LIBRARY}.a")
                message(FATAL_ERROR
                    "Development MVD support requires ${GBASHIELD_FFMPEG_ROOT}/lib/lib${FFMPEG_LIBRARY}.a. "
                    "Run scripts/build-ffmpeg-android-arm64.sh first.")
            endif()
            add_library(gbashield_ffmpeg_${FFMPEG_LIBRARY} STATIC IMPORTED)
            set_target_properties(gbashield_ffmpeg_${FFMPEG_LIBRARY} PROPERTIES
                IMPORTED_LOCATION "${GBASHIELD_FFMPEG_ROOT}/lib/lib${FFMPEG_LIBRARY}.a")
        endforeach()
        set_property(TARGET gbashield_ffmpeg_avcodec PROPERTY
            INTERFACE_LINK_LIBRARIES gbashield_ffmpeg_avutil)
        set_property(TARGET gbashield_ffmpeg_swscale PROPERTY
            INTERFACE_LINK_LIBRARIES gbashield_ffmpeg_avutil)

        target_compile_definitions(citra_core PRIVATE AZAHAR_MVD_FFMPEG=1)
        target_include_directories(citra_core PRIVATE "${GBASHIELD_FFMPEG_ROOT}/include")
        target_link_libraries(citra_core PRIVATE
            gbashield_ffmpeg_avcodec
            gbashield_ffmpeg_swscale
            gbashield_ffmpeg_avutil
            android)
    endif()
endif()
target_link_libraries(citra_libretro_common PRIVATE citra_common citra_core video_core network libretro tsl::robin_map cryptopp fmt Boost::serialization)
if(ENABLE_OPENGL)
    target_link_libraries(citra_libretro_common PRIVATE glad)
endif()
if(ENABLE_VULKAN)
    target_link_libraries(citra_libretro_common PRIVATE sirit vulkan-headers vma)
endif()

add_library(citra_libretro SHARED
        citra_libretro.cpp
        citra_libretro.h
        $<TARGET_OBJECTS:citra_libretro_common>)

set_target_properties(citra_libretro PROPERTIES OUTPUT_NAME "${AZAHAR_LIBRETRO_OUTPUT_NAME}")

if(ANDROID)
    # The frontend is loaded exclusively through the libretro ABI plus GBAShield's small azahar_*
    # extension surface. Keeping the rest of the monolithic core out of .dynsym cuts Android
    # loader work and mapped metadata without changing internal linkage or dead-code decisions.
    set(AZAHAR_LIBRETRO_VERSION_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/azahar_libretro.map")
    target_link_options(citra_libretro PRIVATE
        "-Wl,--version-script=${AZAHAR_LIBRETRO_VERSION_SCRIPT}")
    set_property(TARGET citra_libretro APPEND PROPERTY
        LINK_DEPENDS "${AZAHAR_LIBRETRO_VERSION_SCRIPT}")
endif()

create_target_directory_groups(citra_libretro)

if (BSD STREQUAL "NetBSD")
    include(DisablePaxMprotect)
    disable_pax_mprotect(citra_libretro)
endif()

target_link_libraries(citra_common PRIVATE libretro)
target_link_libraries(citra_core PRIVATE libretro)
target_link_libraries(video_core PRIVATE libretro)
target_link_libraries(audio_core PRIVATE libretro)
target_link_libraries(input_common PRIVATE libretro)
target_compile_definitions(citra_common PRIVATE HAVE_LIBRETRO)
target_compile_definitions(citra_core PRIVATE HAVE_LIBRETRO)
target_compile_definitions(video_core PRIVATE HAVE_LIBRETRO)
target_compile_definitions(audio_core PRIVATE HAVE_LIBRETRO)
target_compile_definitions(input_common PRIVATE HAVE_LIBRETRO)

target_link_libraries(citra_libretro PRIVATE citra_common citra_core)
target_link_libraries(citra_libretro PRIVATE Boost::boost dds-ktx libretro tsl::robin_map)
if(ENABLE_VULKAN)
  target_link_libraries(citra_libretro PRIVATE sirit vulkan-headers vma)
endif()
if(ENABLE_OPENGL)
  target_link_libraries(citra_libretro PRIVATE glad)
endif()
target_link_libraries(citra_libretro PRIVATE ${PLATFORM_LIBRARIES} Threads::Threads)
if(DEFINED LIBRETRO_STATIC)
  target_link_libraries(citra_libretro PRIVATE -static-libstdc++)
endif()

set_target_properties(citra_libretro PROPERTIES PREFIX "")
target_compile_definitions(citra_libretro PRIVATE HAVE_LIBRETRO)

if(ANDROID)
  target_compile_definitions(citra_common PRIVATE HAVE_LIBRETRO_VFS)
  target_compile_definitions(citra_core PRIVATE HAVE_LIBRETRO_VFS)
  target_compile_definitions(video_core PRIVATE HAVE_LIBRETRO_VFS)
  target_compile_definitions(citra_libretro_common PRIVATE USING_GLES HAVE_LIBRETRO_VFS)
  target_compile_definitions(citra_libretro PRIVATE USING_GLES HAVE_LIBRETRO_VFS)
  target_link_libraries(citra_common PRIVATE libretro_common)
  target_link_libraries(citra_core PRIVATE libretro_common)
  target_link_libraries(video_core PRIVATE libretro_common)
  target_link_libraries(citra_libretro_common PRIVATE libretro_common)
  target_link_libraries(citra_libretro PRIVATE libretro_common)
  # Link Android log library for __android_log_print
  target_link_libraries(citra_libretro PRIVATE log)
endif()

if(MINGW)
  target_link_libraries(citra_libretro PRIVATE crypt32)
endif()

if(IOS)
  target_compile_definitions(citra_libretro_common PRIVATE IOS)
  target_compile_definitions(citra_libretro PRIVATE IOS)
  target_link_libraries(citra_libretro PRIVATE "-framework CoreFoundation" "-framework Foundation")
endif()

if (SSE42_COMPILE_OPTION)
  target_compile_definitions(citra_libretro PRIVATE CITRA_HAS_SSE42)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
    CMAKE_SYSTEM_NAME STREQUAL "iOS" OR
    CMAKE_SYSTEM_NAME STREQUAL "tvOS")
  target_link_libraries(citra_libretro PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/libretro.osx.def")
else()
  target_link_libraries(citra_libretro PRIVATE "-Wl,-Bsymbolic")
endif()

add_custom_target(azahar_libretro DEPENDS citra_libretro)

# Tamariba: a check of its rooms (tamariba_rooms.h) without a game, built only when asked for:
#   cmake --build <build> --target tamariba_rooms_test && <build>/bin/Release/tamariba_rooms_test
add_executable(tamariba_rooms_test EXCLUDE_FROM_ALL
        tests/tamariba_rooms_test.cpp
        tamariba_rooms.cpp
        tamariba_rooms.h)
target_link_libraries(tamariba_rooms_test PRIVATE citra_common network enet cryptopp fmt libretro
        Boost::serialization Threads::Threads)
target_compile_definitions(tamariba_rooms_test PRIVATE HAVE_LIBRETRO)

# Tamariba: a check of the console's sockets and HTTP through Tamariba's internet interface
# (core/tamariba_inet.h) against a stand-in exit, built only when asked for (Linux):
#   cmake --build <build> --target tamariba_inet_test && <build>/bin/Release/tamariba_inet_test
add_executable(tamariba_inet_test EXCLUDE_FROM_ALL
        tests/tamariba_inet_test.cpp
        ../core/tamariba_inet.cpp
        ../core/tamariba_inet.h)
target_link_libraries(tamariba_inet_test PRIVATE citra_common httplib fmt Threads::Threads)
