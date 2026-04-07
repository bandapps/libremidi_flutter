# The Flutter tooling requires that developers have CMake 3.10 or later
# installed. You should not increase this version, as doing so will cause
# the plugin to fail to compile for some customers of the plugin.
cmake_minimum_required(VERSION 3.10)

project(libremidi_flutter_library VERSION 0.0.1 LANGUAGES CXX)

# C++20 required by libremidi
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(libremidi_flutter SHARED
  "libremidi_flutter.cpp"
)

# Include libremidi headers (header-only mode)
# Note: Our patched headers in src/libremidi must come FIRST to override libremidi's
target_include_directories(libremidi_flutter PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libremidi/include"
)

# Enable header-only mode for libremidi
target_compile_definitions(libremidi_flutter PRIVATE
  LIBREMIDI_HEADER_ONLY=1
)

set_target_properties(libremidi_flutter PROPERTIES
  PUBLIC_HEADER libremidi_flutter.h
  OUTPUT_NAME "libremidi_flutter"
)

target_compile_definitions(libremidi_flutter PUBLIC DART_SHARED_LIB)

# Platform-specific settings
if(APPLE)
  # macOS and iOS: Link CoreMIDI framework
  target_link_libraries(libremidi_flutter PRIVATE
    "-framework CoreMIDI"
    "-framework CoreFoundation"
    "-framework CoreAudio"
  )
endif()

if(WIN32)
  # Windows MIDI Services (WinMIDI) — primary backend.
  # Falls back to WinUWP at runtime if the service is unavailable.
  # The minimal WinMIDI SDK artifacts are vendored under third_party/winmidi_sdk.
  set(WINMIDI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/winmidi_sdk")
  set(CPPWINRT_OUTDIR "${CMAKE_BINARY_DIR}/cppwinrt-winmidi")

  if(EXISTS "${WINMIDI_DIR}/ref/native/Microsoft.Windows.Devices.Midi2.winmd"
     AND EXISTS "${WINMIDI_DIR}/build/native/include/winmidi")
    set(WINMIDI_AVAILABLE TRUE)
  else()
    message(WARNING "libremidi_flutter: bundled WinMIDI SDK artifacts not found — using WinUWP only.")
    set(WINMIDI_AVAILABLE FALSE)
  endif()

  if(WINMIDI_AVAILABLE)
    file(GLOB _cppwinrt_candidates
      "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/cppwinrt.exe"
    )
    if(_cppwinrt_candidates)
      list(SORT _cppwinrt_candidates)
      list(GET _cppwinrt_candidates -1 CPPWINRT_TOOL)
    endif()

    if(CPPWINRT_TOOL)
      message(STATUS "libremidi_flutter: using cppwinrt at ${CPPWINRT_TOOL}")

      set(_winsdk_base "C:/Program Files (x86)/Windows Kits/10/References")
      file(GLOB _winsdk_versions "${_winsdk_base}/*")
      if(_winsdk_versions)
        list(SORT _winsdk_versions)
        list(GET _winsdk_versions -1 _winsdk_refs)
      endif()
      file(GLOB _winmds "${_winsdk_refs}/*/*/*.winmd")

      set(_rsp "")
      string(APPEND _rsp
        "-input \"${WINMIDI_DIR}/ref/native/Microsoft.Windows.Devices.Midi2.winmd\"\n")
      foreach(_wm IN LISTS _winmds)
        string(APPEND _rsp "-ref \"${_wm}\"\n")
      endforeach()
      file(WRITE "${CMAKE_BINARY_DIR}/cppwinrt-winmidi.rsp" "${_rsp}")

      file(REMOVE_RECURSE "${CPPWINRT_OUTDIR}")
      execute_process(
        COMMAND "${CPPWINRT_TOOL}"
          "@${CMAKE_BINARY_DIR}/cppwinrt-winmidi.rsp"
          -output "${CPPWINRT_OUTDIR}"
        RESULT_VARIABLE _cppwinrt_result
      )

      if(_cppwinrt_result EQUAL 0)
        if(EXISTS "${WINMIDI_DIR}/build/native/include/winmidi/WindowsMidiServicesAppSdkComExtensions.h")
          file(COPY
            "${WINMIDI_DIR}/build/native/include/winmidi/init"
            "${WINMIDI_DIR}/build/native/include/winmidi/WindowsMidiServicesAppSdkComExtensions.h"
            DESTINATION "${CPPWINRT_OUTDIR}/")
        endif()
        message(STATUS "libremidi_flutter: WinMIDI C++ projections generated successfully")
      else()
        message(WARNING "libremidi_flutter: cppwinrt failed (${_cppwinrt_result}). Falling back to pre-packaged headers.")
        file(COPY "${WINMIDI_DIR}/build/native/include/winmidi"
             DESTINATION "${CPPWINRT_OUTDIR}/")
      endif()
    else()
      message(STATUS "libremidi_flutter: cppwinrt not found — using pre-packaged WinMIDI headers")
      file(COPY "${WINMIDI_DIR}/build/native/include/winmidi"
           DESTINATION "${CPPWINRT_OUTDIR}/")
    endif()

    target_include_directories(libremidi_flutter PRIVATE
      "${CPPWINRT_OUTDIR}"
    )
  endif()

  if(WINMIDI_AVAILABLE)
    target_compile_definitions(libremidi_flutter PRIVATE
      LIBREMIDI_WINMIDI=1
      LIBREMIDI_WINUWP=1
      LIBREMIDI_NO_WINMM=1
      LIBREMIDI_NO_ALSA=1
      LIBREMIDI_NO_JACK=1
      LIBREMIDI_NO_PIPEWIRE=1
      LIBREMIDI_NO_COREMIDI=1
      LIBREMIDI_NO_ANDROID=1
    )
  else()
    target_compile_definitions(libremidi_flutter PRIVATE
      LIBREMIDI_WINUWP=1
      LIBREMIDI_NO_WINMM=1
      LIBREMIDI_NO_ALSA=1
      LIBREMIDI_NO_JACK=1
      LIBREMIDI_NO_PIPEWIRE=1
      LIBREMIDI_NO_COREMIDI=1
      LIBREMIDI_NO_ANDROID=1
    )
  endif()

  # Link WinRT, cfgmgr32 (device info), RuntimeObject
  target_link_libraries(libremidi_flutter PRIVATE windowsapp cfgmgr32 RuntimeObject)
endif()

if(UNIX AND NOT APPLE AND NOT ANDROID)
  # Linux: Enable ONLY ALSA Sequencer backend, disable all others
  include(CheckIncludeFileCXX)
  check_include_file_cxx("sys/eventfd.h" LIBREMIDI_HAS_EVENTFD)
  check_include_file_cxx("sys/timerfd.h" LIBREMIDI_HAS_TIMERFD)

  find_package(ALSA REQUIRED)

  if(ALSA_FOUND AND LIBREMIDI_HAS_EVENTFD AND LIBREMIDI_HAS_TIMERFD)
    message(STATUS "libremidi_flutter: ALSA Sequencer backend enabled")

    # Backend selection
    target_compile_definitions(libremidi_flutter PRIVATE
      LIBREMIDI_ALSA=1
      # Explicitly disable other backends
      LIBREMIDI_NO_JACK=1
      LIBREMIDI_NO_PIPEWIRE=1
      LIBREMIDI_NO_COREMIDI=1
      LIBREMIDI_NO_WINMM=1
      LIBREMIDI_NO_ANDROID=1
    )

    # Link ALSA and dl (for dynamic loading)
    target_link_libraries(libremidi_flutter PRIVATE ${ALSA_LIBRARIES} ${CMAKE_DL_LIBS})
    target_include_directories(libremidi_flutter PRIVATE ${ALSA_INCLUDE_DIRS})

    # Optional: udev for extended device info (manufacturer, product, serial)
    find_path(UDEV_INCLUDE_DIR libudev.h)
    if(UDEV_INCLUDE_DIR)
      message(STATUS "libremidi_flutter: udev enabled (extended device info)")
      target_compile_definitions(libremidi_flutter PRIVATE LIBREMIDI_HAS_UDEV=1)
      target_include_directories(libremidi_flutter PRIVATE ${UDEV_INCLUDE_DIR})
    else()
      message(STATUS "libremidi_flutter: udev not found (basic device info only)")
    endif()
  else()
    message(FATAL_ERROR "libremidi_flutter: Linux requires ALSA, sys/eventfd.h, and sys/timerfd.h")
  endif()
endif()

if(ANDROID)
  # Android: Enable ONLY Android backend, disable all others
  target_compile_definitions(libremidi_flutter PRIVATE
    LIBREMIDI_ANDROID=1
    # Explicitly disable other backends
    LIBREMIDI_NO_ALSA=1
    LIBREMIDI_NO_JACK=1
    LIBREMIDI_NO_PIPEWIRE=1
    LIBREMIDI_NO_COREMIDI=1
    LIBREMIDI_NO_WINMM=1
  )

  # Add JNI shim and patched libremidi helpers (uses cached ClassLoader)
  target_sources(libremidi_flutter PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/jni_shim.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/android_helpers.cpp"
  )

  # Link AMidi and other Android libraries
  find_library(ANDROID_LOG_LIB log)
  find_library(ANDROID_AMIDI_LIB amidi)
  find_library(ANDROID_LIB android)
  find_library(ANDROID_DL_LIB dl)
  target_link_libraries(libremidi_flutter PRIVATE
    ${ANDROID_AMIDI_LIB}
    ${ANDROID_LOG_LIB}
    ${ANDROID_LIB}
    ${ANDROID_DL_LIB}
  )

  # Support Android 15 16k page size
  target_link_options(libremidi_flutter PRIVATE "-Wl,-z,max-page-size=16384")
endif()
