cmake_minimum_required(VERSION 3.20)
# The oldest macOS the app supports (also LSMinimumSystemVersion in packaging/Info.plist, FFmpeg's
# -mmacosx-version-min in setup.sh, and the player's build in tools/installer/build_game.sh). Without it the build targets the build machine's own macOS.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0 CACHE STRING "Minimum macOS" FORCE)
endif()
project(Peggle2Mac CXX C OBJCXX)
set(FFMPEG_DIR ${CMAKE_SOURCE_DIR}/third_party/ffmpeg-install)

set(CMAKE_CXX_STANDARD 20)
add_subdirectory(third_party/XenonRecomp)

add_executable(xexdump tools/xexdump/main.cpp)
target_link_libraries(xexdump PRIVATE XenonUtils)

add_executable(install_game tools/install_game/main.cpp runtime/game_import.cpp)
target_include_directories(install_game PRIVATE runtime third_party/XenonRecomp/thirdparty/fmt/include)
target_link_libraries(install_game PRIVATE fmt::fmt)

add_executable(disas tools/disas/main.cpp)
target_link_libraries(disas PRIVATE XenonUtils)

# Recompiled game code. Generated into out/ppc by XenonRecomp from the user's own Default.xex.
set(PEGGLE2_PPC_DIR ${CMAKE_SOURCE_DIR}/out/ppc)
file(GLOB PEGGLE2_PPC_SOURCES CONFIGURE_DEPENDS ${PEGGLE2_PPC_DIR}/ppc_recomp.*.cpp ${PEGGLE2_PPC_DIR}/ppc_func_mapping.cpp)
if(PEGGLE2_PPC_SOURCES)
    add_library(Peggle2Recomp OBJECT ${PEGGLE2_PPC_SOURCES})
    target_include_directories(Peggle2Recomp PRIVATE
        ${PEGGLE2_PPC_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/XenonRecomp/XenonUtils
        ${CMAKE_SOURCE_DIR}/third_party/XenonRecomp/thirdparty/simde)
    target_compile_options(Peggle2Recomp PRIVATE
        -include ${CMAKE_SOURCE_DIR}/runtime/recomp_hooks.h
        -fno-strict-aliasing
        -Wno-unused-label -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable
        -Wno-switch -Wno-null-arithmetic -Wno-tautological-overlap-compare)
endif()

add_executable(fnanalyze tools/fnanalyze/main.cpp)
target_link_libraries(fnanalyze PRIVATE LibXenonAnalyse XenonUtils)

# Native runtime: everything but the recompiled code, which it calls by name. Prebuilt into the installer,
# which links it with code recompiled on the player's Mac.
file(GLOB_RECURSE PEGGLE2_RUNTIME_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/*.cpp ${CMAKE_SOURCE_DIR}/runtime/*.mm)
list(FILTER PEGGLE2_RUNTIME_SOURCES EXCLUDE REGEX "/runtime/gpu/xenos/")
add_library(Peggle2Runtime STATIC ${PEGGLE2_RUNTIME_SOURCES})
target_include_directories(Peggle2Runtime PRIVATE
    ${CMAKE_SOURCE_DIR}/config
    ${CMAKE_SOURCE_DIR}/third_party/XenonRecomp/XenonUtils
    ${CMAKE_SOURCE_DIR}/third_party/XenonRecomp/thirdparty/simde)
target_compile_options(Peggle2Runtime PRIVATE -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(Peggle2Runtime PUBLIC XenonUtils Xenos fmt::fmt)
set(PEGGLE2_FRAMEWORKS "-framework AudioToolbox" "-framework ImageIO" "-framework CoreGraphics" "-framework CoreAudio"
    "-framework AppKit" "-framework CoreHaptics" "-framework IOKit" "-framework GameController" "-framework Metal"
    "-framework QuartzCore")

# The app, from this machine's game/ (developers).
if(PEGGLE2_PPC_SOURCES)
    add_executable(Peggle2 $<TARGET_OBJECTS:Peggle2Recomp>)
    set_target_properties(Peggle2 PROPERTIES MACOSX_BUNDLE TRUE MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/Info.plist)
    # FFmpeg ships inside the bundle, the icon is composed from the game's artwork, then an ad-hoc signature
    # so it runs on Apple silicon after copying.
    set_target_properties(Peggle2 PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "@executable_path/../Frameworks")
    add_custom_command(TARGET Peggle2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_CONTENT_DIR:Peggle2>/Frameworks
        COMMAND ${CMAKE_COMMAND} -E copy ${FFMPEG_DIR}/lib/libavcodec.58.dylib ${FFMPEG_DIR}/lib/libavutil.56.dylib $<TARGET_BUNDLE_CONTENT_DIR:Peggle2>/Frameworks/
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_CONTENT_DIR:Peggle2>/Resources
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md $<TARGET_BUNDLE_CONTENT_DIR:Peggle2>/Resources/
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/packaging/licenses $<TARGET_BUNDLE_CONTENT_DIR:Peggle2>/Resources/licenses
        COMMAND ${CMAKE_SOURCE_DIR}/tools/make_icon.sh $<TARGET_BUNDLE_DIR:Peggle2> ${CMAKE_SOURCE_DIR}/game
        COMMAND ${CMAKE_SOURCE_DIR}/tools/sign_bundle.sh $<TARGET_BUNDLE_DIR:Peggle2>
        VERBATIM)
    # Force-loaded: the runtime's hooks replace weak definitions in the recompiled code, and a plain archive
    # member would not be pulled in for a symbol already defined.
    target_link_libraries(Peggle2 PRIVATE -Wl,-force_load,$<TARGET_FILE:Peggle2Runtime> Peggle2Runtime ${PEGGLE2_FRAMEWORKS})
endif()

# The installer: a small app that installs the player's game, recompiles it with the bundled XenonRecomp and
# links it with the prebuilt runtime on their Mac (tools/installer/build_game.sh), producing Peggle 2.app.
add_executable(Peggle2Installer MACOSX_BUNDLE tools/installer/main.mm runtime/game_import.cpp runtime/platform/folders.mm)
set_target_properties(Peggle2Installer PROPERTIES OUTPUT_NAME "Peggle 2 Installer"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/Installer-Info.plist)
target_include_directories(Peggle2Installer PRIVATE runtime third_party/XenonRecomp/thirdparty/fmt/include)
target_compile_options(Peggle2Installer PRIVATE -fobjc-arc)
target_link_libraries(Peggle2Installer PRIVATE fmt::fmt "-framework AppKit")
add_custom_command(TARGET Peggle2Installer POST_BUILD
    COMMAND ${CMAKE_SOURCE_DIR}/tools/installer/make_sdk.sh $<TARGET_BUNDLE_DIR:Peggle2Installer> ${CMAKE_SOURCE_DIR}
        $<TARGET_FILE:Peggle2Runtime> $<TARGET_FILE:Xenos> $<TARGET_FILE:XenonUtils> $<TARGET_FILE:fmt::fmt>
        $<TARGET_FILE:XenonRecomp> ${FFMPEG_DIR}/lib
    VERBATIM)
add_dependencies(Peggle2Installer Peggle2Runtime XenonRecomp)
# Repack when anything the SDK carries changes.
set_source_files_properties(tools/installer/main.mm PROPERTIES OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/installer/build_game.sh;${CMAKE_SOURCE_DIR}/tools/installer/make_sdk.sh;${CMAKE_SOURCE_DIR}/tools/make_icon.sh;${CMAKE_SOURCE_DIR}/runtime/recomp_hooks.h;${CMAKE_SOURCE_DIR}/config/Peggle2.toml;${CMAKE_SOURCE_DIR}/config/switch_tables.toml;${CMAKE_SOURCE_DIR}/packaging/Info.plist;${CMAKE_BINARY_DIR}/libPeggle2Runtime.a")

# Xenos shader microcode support (Xenia, BSD-3-Clause).
file(GLOB XENOS_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/gpu/xenos/xenia/gpu/*.cc ${CMAKE_SOURCE_DIR}/runtime/gpu/xenos/xenia/ui/*.cc
    ${CMAKE_SOURCE_DIR}/runtime/gpu/xenos/xenia/apu/*.cc ${CMAKE_SOURCE_DIR}/runtime/gpu/xenos/xenia/base/*.cc)
add_library(Xenos STATIC ${XENOS_SOURCES})
target_include_directories(Xenos SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/runtime/gpu/xenos ${CMAKE_SOURCE_DIR}/third_party/XenonRecomp/thirdparty/xxHash)
# XMA decoding: Xenia's FFmpeg fork (LGPL-2.1), built as shared libraries by setup.sh and bundled.
target_include_directories(Xenos SYSTEM PUBLIC ${FFMPEG_DIR}/include)
target_link_libraries(Xenos PUBLIC fmt::fmt ${FFMPEG_DIR}/lib/libavcodec.dylib ${FFMPEG_DIR}/lib/libavutil.dylib)
target_compile_options(Xenos PRIVATE -Wno-everything)

add_executable(xenos_disasm tools/xenos_disasm/main.cpp)
target_link_libraries(xenos_disasm PRIVATE Xenos)

add_executable(msl_check tools/msl_check/main.mm runtime/gpu/msl_translator.cpp)
target_link_libraries(msl_check PRIVATE Xenos "-framework Metal" "-framework Foundation")
target_compile_options(msl_check PRIVATE -fobjc-arc)
