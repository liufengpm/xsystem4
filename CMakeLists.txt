cmake_minimum_required(VERSION 3.14)
project(xsystem4 LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)
find_package(PkgConfig QUIET)

option(XSYSTEM4_BUILD_PLUGIN "Build xsystem4 as a VintagePomelo plugin" OFF)
option(XSYSTEM4_USE_HARMONY_PREBUILT_DEPS "Use explicit Harmony prebuilt shared libraries" OFF)
set(XSYSTEM4_AUDIO_BACKEND "sndfile" CACHE STRING "Audio backend to use (sndfile or ffmpeg)")
set_property(CACHE XSYSTEM4_AUDIO_BACKEND PROPERTY STRINGS sndfile ffmpeg)

if (NOT XSYSTEM4_AUDIO_BACKEND STREQUAL "sndfile" AND NOT XSYSTEM4_AUDIO_BACKEND STREQUAL "ffmpeg")
  message(FATAL_ERROR "XSYSTEM4_AUDIO_BACKEND must be either 'sndfile' or 'ffmpeg'")
endif()

if (XSYSTEM4_USE_HARMONY_PREBUILT_DEPS)
  foreach(_required_var
      HARMONY_SDL2_INCLUDE_DIR
      HARMONY_SDL2_LIB_DIR
      HARMONY_DEPS_INCLUDE_DIR
      HARMONY_DEPS_LIB_DIR)
    if (NOT DEFINED ${_required_var} OR "${${_required_var}}" STREQUAL "")
      message(FATAL_ERROR "${_required_var} must be set when XSYSTEM4_USE_HARMONY_PREBUILT_DEPS=ON")
    endif()
  endforeach()
  if (XSYSTEM4_AUDIO_BACKEND STREQUAL "ffmpeg")
    foreach(_required_var HARMONY_FFMPEG_INCLUDE_DIR HARMONY_FFMPEG_LIB_DIR)
      if (NOT DEFINED ${_required_var} OR "${${_required_var}}" STREQUAL "")
        message(FATAL_ERROR "${_required_var} must be set when XSYSTEM4_AUDIO_BACKEND=ffmpeg")
      endif()
    endforeach()
  endif()
endif()

set(XSYSTEM4_VENDORED_CGLM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/subprojects/cglm")
if (EXISTS "${XSYSTEM4_VENDORED_CGLM_DIR}/CMakeLists.txt")
  set(CGLM_USE_TEST OFF CACHE BOOL "" FORCE)
  if (XSYSTEM4_BUILD_PLUGIN)
    set(CGLM_SHARED OFF CACHE BOOL "" FORCE)
    set(CGLM_STATIC ON CACHE BOOL "" FORCE)
  else()
    set(CGLM_SHARED ON CACHE BOOL "" FORCE)
    set(CGLM_STATIC OFF CACHE BOOL "" FORCE)
  endif()
  message(STATUS "Using vendored cglm from ${XSYSTEM4_VENDORED_CGLM_DIR}")
  add_subdirectory("${XSYSTEM4_VENDORED_CGLM_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/subprojects/cglm")
else()
  if (XSYSTEM4_BUILD_PLUGIN)
    set(CGLM_SHARED OFF CACHE BOOL "" FORCE)
    set(CGLM_STATIC ON CACHE BOOL "" FORCE)
  endif()
  FetchContent_Declare(
    cglm
    URL https://github.com/recp/cglm/archive/refs/tags/v0.9.2.tar.gz
    URL_HASH SHA1=cb8472aa8c2ab67b66378dbaf10c2c7368d4e4c3
  )
  FetchContent_MakeAvailable(cglm)
endif()

if (XSYSTEM4_USE_HARMONY_PREBUILT_DEPS)
  set(XSYSTEM4_EXTRA_INCLUDE_DIRS
    ${HARMONY_SDL2_INCLUDE_DIR}
    ${HARMONY_DEPS_INCLUDE_DIR}
    ${HARMONY_DEPS_INCLUDE_DIR}/freetype2)
  set(XSYSTEM4_SDL_LIBRARY "${HARMONY_SDL2_LIB_DIR}/libSDL2.so")
  set(XSYSTEM4_FREETYPE_LIBRARY "${HARMONY_SDL2_LIB_DIR}/libfreetype.so")
  set(XSYSTEM4_ZLIB_LIBRARY "${HARMONY_SDL2_LIB_DIR}/libz.so")
  set(XSYSTEM4_FFI_LIBRARY "${HARMONY_SDL2_LIB_DIR}/libffi.so")

  if (XSYSTEM4_AUDIO_BACKEND STREQUAL "sndfile")
    message(FATAL_ERROR "sndfile backend is not available with Harmony prebuilt dependencies")
  endif()

  list(APPEND XSYSTEM4_EXTRA_INCLUDE_DIRS ${HARMONY_FFMPEG_INCLUDE_DIR})
  set(XSYSTEM4_AUDIO_LINK_LIBRARIES
    "${HARMONY_FFMPEG_LIB_DIR}/libavcodec.so"
    "${HARMONY_FFMPEG_LIB_DIR}/libavformat.so"
    "${HARMONY_FFMPEG_LIB_DIR}/libavutil.so"
    "${HARMONY_FFMPEG_LIB_DIR}/libswresample.so")
  set(XSYSTEM4_GL_LINK_LIBRARIES GLESv3)

  set(LIBSYS4_USE_PREBUILT_HARMONY_DEPS ON CACHE BOOL "" FORCE)
  set(HARMONY_DEPS_INCLUDE_DIR "${HARMONY_DEPS_INCLUDE_DIR}" CACHE PATH "" FORCE)
  set(HARMONY_DEPS_LIB_DIR "${HARMONY_DEPS_LIB_DIR}" CACHE PATH "" FORCE)
  set(LIBSYS4_ZLIB_LIBRARY "${HARMONY_SDL2_LIB_DIR}/libz.so" CACHE FILEPATH "" FORCE)
  if (DEFINED HARMONY_WEBP_INCLUDE_DIR AND NOT "${HARMONY_WEBP_INCLUDE_DIR}" STREQUAL "")
    set(LIBSYS4_WEBP_INCLUDE_DIR "${HARMONY_WEBP_INCLUDE_DIR}" CACHE PATH "" FORCE)
  endif()
else()
  if (DEFINED CMAKE_STAGING_PREFIX AND NOT "${CMAKE_STAGING_PREFIX}" STREQUAL "" AND EXISTS "${CMAKE_STAGING_PREFIX}/lib/libffi.a")
    add_library(ffi STATIC IMPORTED)
    set_target_properties(ffi PROPERTIES
      IMPORTED_LOCATION ${CMAKE_STAGING_PREFIX}/lib/libffi.a
      INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_STAGING_PREFIX}/include)
    set(XSYSTEM4_FFI_LIBRARY ffi)
  elseif (PkgConfig_FOUND)
    pkg_check_modules(XSYSTEM4_LIBFFI REQUIRED IMPORTED_TARGET libffi)
    set(XSYSTEM4_FFI_LIBRARY PkgConfig::XSYSTEM4_LIBFFI)
  else()
    message(FATAL_ERROR "libffi could not be resolved for the native CMake build")
  endif()

  find_package(Freetype REQUIRED)
  find_package(SDL2 REQUIRED)
  find_package(ZLIB REQUIRED)

  set(XSYSTEM4_SDL_LIBRARY SDL2::SDL2)
  set(XSYSTEM4_FREETYPE_LIBRARY Freetype::Freetype)
  set(XSYSTEM4_ZLIB_LIBRARY ZLIB::ZLIB)

  if (XSYSTEM4_AUDIO_BACKEND STREQUAL "sndfile")
    find_package(SndFile QUIET)
    if (TARGET SndFile::sndfile)
      set(XSYSTEM4_AUDIO_LINK_LIBRARIES SndFile::sndfile)
    elseif (PkgConfig_FOUND)
      pkg_check_modules(XSYSTEM4_SNDFILE REQUIRED IMPORTED_TARGET sndfile)
      set(XSYSTEM4_AUDIO_LINK_LIBRARIES PkgConfig::XSYSTEM4_SNDFILE)
    else()
      message(FATAL_ERROR "sndfile backend requested, but neither FindSndFile nor pkg-config could resolve it")
    endif()
  else()
    if (NOT PkgConfig_FOUND)
      message(FATAL_ERROR "ffmpeg audio backend requires pkg-config on native CMake builds")
    endif()
    pkg_check_modules(XSYSTEM4_AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
    pkg_check_modules(XSYSTEM4_AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
    pkg_check_modules(XSYSTEM4_AVUTIL REQUIRED IMPORTED_TARGET libavutil)
    pkg_check_modules(XSYSTEM4_SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)
    set(XSYSTEM4_AUDIO_LINK_LIBRARIES
      PkgConfig::XSYSTEM4_AVCODEC
      PkgConfig::XSYSTEM4_AVFORMAT
      PkgConfig::XSYSTEM4_AVUTIL
      PkgConfig::XSYSTEM4_SWRESAMPLE)
  endif()

  if (CMAKE_SYSTEM_NAME MATCHES "OHOS")
    set(XSYSTEM4_GL_LINK_LIBRARIES GLESv3)
  elseif (PkgConfig_FOUND)
    pkg_check_modules(XSYSTEM4_GLES QUIET IMPORTED_TARGET glesv2)
    if (XSYSTEM4_GLES_FOUND)
      set(XSYSTEM4_GL_LINK_LIBRARIES PkgConfig::XSYSTEM4_GLES)
      pkg_check_modules(XSYSTEM4_EGL QUIET IMPORTED_TARGET egl)
      if (XSYSTEM4_EGL_FOUND)
        list(APPEND XSYSTEM4_GL_LINK_LIBRARIES PkgConfig::XSYSTEM4_EGL)
      endif()
    elseif (WIN32)
      # MSYS2 ANGLE exposes a single angleproject.pc target carrying the GLES runtime.
      pkg_check_modules(XSYSTEM4_ANGLE REQUIRED IMPORTED_TARGET angleproject)
      set(XSYSTEM4_GL_LINK_LIBRARIES PkgConfig::XSYSTEM4_ANGLE)
    else()
      message(FATAL_ERROR "OpenGL ES libraries could not be resolved (tried glesv2)")
    endif()
  else()
    message(FATAL_ERROR "OpenGL ES libraries require pkg-config on native CMake builds")
  endif()
endif()

set(XSYSTEM4_MOVIE_SOURCE src/movie_plmpeg.c)
set(XSYSTEM4_MOVIE_BACKEND plmpeg)
set(XSYSTEM4_MOVIE_LINK_LIBRARIES)

if (XSYSTEM4_AUDIO_BACKEND STREQUAL "ffmpeg")
  if (XSYSTEM4_USE_HARMONY_PREBUILT_DEPS)
    set(_xsystem4_swscale_library "${HARMONY_FFMPEG_LIB_DIR}/libswscale.so")
    if (EXISTS "${_xsystem4_swscale_library}")
      set(XSYSTEM4_MOVIE_SOURCE src/movie_ffmpeg.c)
      set(XSYSTEM4_MOVIE_BACKEND ffmpeg)
      list(APPEND XSYSTEM4_MOVIE_LINK_LIBRARIES "${_xsystem4_swscale_library}")
    else()
      message(WARNING "libswscale.so not found under ${HARMONY_FFMPEG_LIB_DIR}; falling back to plmpeg movie backend")
    endif()
  else()
    pkg_check_modules(XSYSTEM4_SWSCALE QUIET IMPORTED_TARGET libswscale)
    if (XSYSTEM4_SWSCALE_FOUND)
      set(XSYSTEM4_MOVIE_SOURCE src/movie_ffmpeg.c)
      set(XSYSTEM4_MOVIE_BACKEND ffmpeg)
      list(APPEND XSYSTEM4_MOVIE_LINK_LIBRARIES PkgConfig::XSYSTEM4_SWSCALE)
    else()
      message(WARNING "libswscale not found; falling back to plmpeg movie backend")
    endif()
  endif()
endif()

message(STATUS "xsystem4 movie backend: ${XSYSTEM4_MOVIE_BACKEND}")

add_subdirectory(subprojects/libsys4)

set(XSYSTEM4_TARGET_NAME xsystem4)
if (XSYSTEM4_BUILD_PLUGIN)
  set(XSYSTEM4_TARGET_NAME xsystem4_plugin)
endif()

if (XSYSTEM4_BUILD_PLUGIN)
  add_library(${XSYSTEM4_TARGET_NAME} SHARED)
else()
  add_executable(${XSYSTEM4_TARGET_NAME})
endif()

target_compile_definitions(${XSYSTEM4_TARGET_NAME} PRIVATE
  _DEFAULT_SOURCE
  USE_GLES
  $<$<BOOL:${XSYSTEM4_BUILD_PLUGIN}>:XSYSTEM4_BUILD_PLUGIN>
  $<$<NOT:$<BOOL:${XSYSTEM4_BUILD_PLUGIN}>>:SDL_MAIN_HANDLED>
  )

target_include_directories(${XSYSTEM4_TARGET_NAME} PRIVATE
  include
  ${CMAKE_CURRENT_BINARY_DIR}
  ${XSYSTEM4_EXTRA_INCLUDE_DIRS})

execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE VCS_TAG
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE VCS_TAG_RESULT
  ERROR_QUIET)
if (NOT VCS_TAG_RESULT EQUAL 0 OR VCS_TAG STREQUAL "")
  set(VCS_TAG unknown)
endif()
configure_file(src/version.h.in version.h)

set(XSYSTEM4_AUDIO_MIXER_SOURCE src/audio_mixer.c)
if (XSYSTEM4_AUDIO_BACKEND STREQUAL "ffmpeg")
  set(XSYSTEM4_AUDIO_MIXER_SOURCE src/audio_mixer_ffmpeg.c)
endif()

target_sources(${XSYSTEM4_TARGET_NAME} PRIVATE
  src/audio.c
  src/audio_meta.c
  ${XSYSTEM4_AUDIO_MIXER_SOURCE}
  src/asset_manager.c
  src/base64.c
  src/cJSON.c
  src/draw.c
  src/effect.c
  src/ffi.c
  src/font_freetype.c
  src/font_fnl.c
  src/format.c
  src/hacks.c
  src/heap.c
  src/icon.c
  src/id_pool.c
  src/input.c
  src/json.c
  ${XSYSTEM4_MOVIE_SOURCE}
  src/msgqueue.c
  src/page.c
  src/resume.c
  src/savedata.c
  src/scene.c
  src/screenshot.c
  src/sprite.c
  src/swf.c
  src/system4.c
  src/text.c
  src/util.c
  src/video.c
  src/vm.c

  src/3d/collision.c
  src/3d/debug.c
  src/3d/model.c
  src/3d/parser.c
  src/3d/particle.c
  src/3d/reign.c
  src/3d/renderer.c

  src/dungeon/dgn.c
  src/dungeon/dtx.c
  src/dungeon/dungeon.c
  src/dungeon/generator.c
  src/dungeon/map.c
  src/dungeon/mtrand43.c
  src/dungeon/polyobj.c
  src/dungeon/renderer.c
  src/dungeon/skybox.c
  src/dungeon/tes.c

  src/parts/construction.c
  src/parts/debug.c
  src/parts/flash.c
  src/parts/flat.c
  src/parts/input.c
  src/parts/layoutbox.c
  src/parts/message.c
  src/parts/motion.c
  src/parts/parts.c
  src/parts/render.c
  src/parts/text.c
  src/parts/save.c

  src/hll/ACXLoader.c
  src/hll/ADVSYS.c
  src/hll/AFAFactory.c
  src/hll/AliceLogo.c
  src/hll/AliceLogo2.c
  src/hll/AliceLogo3.c
  src/hll/AliceLogo4.c
  src/hll/AliceLogo5.c
  src/hll/AnteaterADVEngine.c
  src/hll/Array.c
  src/hll/BanMisc.c
  src/hll/Bitarray.c
  src/hll/CalcTable.c
  src/hll/CGManager.c
  src/hll/CharSpriteManager.c
  src/hll/ChipmunkSpriteEngine.c
  src/hll/ChrLoader.c
  src/hll/CommonSystemData.c
  src/hll/Confirm.c
  src/hll/Confirm2.c
  src/hll/Confirm3.c
  src/hll/CrayfishLogViewer.c
  src/hll/Cursor.c
  src/hll/DALKDemo.c
  src/hll/DALKEDemo.c
  src/hll/Data.c
  src/hll/DataFile.c
  src/hll/Discord.c
  src/hll/DrawDungeon.c
  src/hll/DrawEffect.c
  src/hll/DrawGraph.c
  src/hll/DrawMovie.c
  src/hll/DrawNumeral.c
  src/hll/DrawPluginManager.c
  src/hll/DrawRain.c
  src/hll/DrawRipple.c
  src/hll/DrawSimpleText.c
  src/hll/DrawSnow.c
  src/hll/File.c
  src/hll/File2.c
  src/hll/FileOperation.c
  src/hll/FillAngle.c
  src/hll/GoatGUIEngine.c
  src/hll/Gpx2Plus.c
  src/hll/GUIEngine.c
  src/hll/HTTPDownloader.c
  src/hll/iarray.c
  src/hll/IbisInputEngine.c
  src/hll/InputDevice.c
  src/hll/InputString.c
  src/hll/KiwiSoundEngine.c
  src/hll/LoadCG.c
  src/hll/MADLoader.c
  src/hll/MainEXFile.c
  src/hll/MainSurface.c
  src/hll/MamanyoDemo.c
  src/hll/MamanyoSDemo.c
  src/hll/MarmotModelEngine.c
  src/hll/Math.c
  src/hll/MapLoader.c
  src/hll/MenuMsg.c
  src/hll/MonsterInfo.c
  src/hll/MsgLogManager.c
  src/hll/MsgLogViewer.c
  src/hll/MsgSkip.c
  src/hll/MusicSystem.c
  src/hll/NewFont.c
  src/hll/OutputLog.c
  src/hll/P3MapSprite.c
  src/hll/P3SquareSprite.c
  src/hll/PassRegister.c
  src/hll/PastelChime2.c
  src/hll/PartsEngine.c
  src/hll/PixelRestore.c
  src/hll/PlayDemo.c
  src/hll/PlayMovie.c
  src/hll/ReignEngine.c
  src/hll/SACT2.c
  src/hll/SengokuRanceFont.c
  src/hll/SoundFilePlayer.c
  src/hll/Sound2ex.c
  src/hll/StoatSpriteEngine.c
  src/hll/StretchHelper.c
  src/hll/SystemService.c
  src/hll/SystemServiceEx.c
  src/hll/Timer.c
  src/hll/Toushin3Loader.c
  src/hll/vmAnime.c
  src/hll/vmArray.c
  src/hll/vmCG.c
  src/hll/vmChrLoader.c
  src/hll/vmCursor.c
  src/hll/vmDalkGaiden.c
  src/hll/vmData.c
  src/hll/vmDialog.c
  src/hll/vmDrawGauge.c
  src/hll/vmDrawMsg.c
  src/hll/vmDrawNumber.c
  src/hll/vmFile.c
  src/hll/vmGraph.c
  src/hll/vmGraphQuake.c
  src/hll/vmKey.c
  src/hll/vmMapLoader.c
  src/hll/vmMsgLog.c
  src/hll/vmMusic.c
  src/hll/vmSound.c
  src/hll/vmSprite.c
  src/hll/vmString.c
  src/hll/vmSurface.c
  src/hll/vmSystem.c
  src/hll/vmTimer.c
  src/hll/ValueEncryption.c
  src/hll/VSFile.c
  )

if (XSYSTEM4_BUILD_PLUGIN)
  target_sources(${XSYSTEM4_TARGET_NAME} PRIVATE src/xsystem4_plugin_entry.c)
endif()

target_link_libraries(${XSYSTEM4_TARGET_NAME} PRIVATE
  m
  ${XSYSTEM4_ZLIB_LIBRARY}
  ${XSYSTEM4_SDL_LIBRARY}
  ${XSYSTEM4_FREETYPE_LIBRARY}
  ${XSYSTEM4_FFI_LIBRARY}
  ${XSYSTEM4_GL_LINK_LIBRARIES}
  cglm
  ${XSYSTEM4_AUDIO_LINK_LIBRARIES}
  ${XSYSTEM4_MOVIE_LINK_LIBRARIES}
  sys4)

if (CMAKE_SYSTEM_NAME MATCHES "OHOS")
  target_compile_definitions(${XSYSTEM4_TARGET_NAME} PRIVATE XSYSTEM4_HOST_UTF8)
  if (XSYSTEM4_BUILD_PLUGIN)
    target_compile_definitions(${XSYSTEM4_TARGET_NAME} PRIVATE XSYS4_DATA_DIR="xsystem4")
  endif()
  target_link_libraries(${XSYSTEM4_TARGET_NAME} PRIVATE hilog_ndk.z dl)
  set_target_properties(${XSYSTEM4_TARGET_NAME} PROPERTIES
    BUILD_RPATH "\$ORIGIN"
    INSTALL_RPATH "\$ORIGIN"
    BUILD_WITH_INSTALL_RPATH TRUE)

  # Normalize DT_NEEDED: replace absolute Windows paths with bare sonames after linking.
  # libz is linked via its full path (HARMONY_SDL2_LIB_DIR/libz.so) which embeds the
  # absolute path into DT_NEEDED; patch_elf_soname.py replaces it in-place.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if (Python3_FOUND AND XSYSTEM4_BUILD_PLUGIN)
    set(_PATCH_ELF_SCRIPT "${CMAKE_SOURCE_DIR}/../../patch_elf_soname.py")
    if (NOT EXISTS "${_PATCH_ELF_SCRIPT}")
      set(_PATCH_ELF_SCRIPT "${CMAKE_SOURCE_DIR}/../../../patch_elf_soname.py")
    endif()
    if (EXISTS "${_PATCH_ELF_SCRIPT}")
      add_custom_command(TARGET ${XSYSTEM4_TARGET_NAME} POST_BUILD
        COMMAND ${Python3_EXECUTABLE} "${_PATCH_ELF_SCRIPT}"
          "$<TARGET_FILE:${XSYSTEM4_TARGET_NAME}>"
          "$<TARGET_FILE:${XSYSTEM4_TARGET_NAME}>"
          "${HARMONY_SDL2_LIB_DIR}/libz.so"
          "libz.so"
        COMMENT "Normalizing libz DT_NEEDED absolute path in xsystem4 plugin"
        VERBATIM)
    endif()
  endif()
endif()

if (XSYSTEM4_BUILD_PLUGIN)
  set_target_properties(${XSYSTEM4_TARGET_NAME} PROPERTIES
    OUTPUT_NAME "xsystem4_plugin"
    PREFIX "lib")
endif()

target_compile_options(${XSYSTEM4_TARGET_NAME} PRIVATE -Wno-unused-parameter)

install(TARGETS ${XSYSTEM4_TARGET_NAME})
