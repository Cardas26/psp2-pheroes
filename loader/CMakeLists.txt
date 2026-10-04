cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()
project(palmheroes C ASM)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Palm Heroes")
set(VITA_TITLEID "PHEROES01")
set(VITA_VERSION "01.00")
option(DIAGNOSTICS "write ux0:/data/palmheroes/debug.log: the build id, patches, faults and a heartbeat" OFF)
set(GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../game" CACHE PATH "tools/prepare_game.py's output: Data/, Maps/, ph.img")

if(NOT PHEROES_BUILD_ID)
  execute_process(COMMAND git describe --tags --always --dirty
                  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                  OUTPUT_VARIABLE _id OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT _id)
    set(_id unknown)
  endif()
  set(PHEROES_BUILD_ID "psp2-pheroes-${_id}")
endif()
if(DIAGNOSTICS)
  set(_profile diagnostics)
else()
  set(_profile production)
endif()
message(STATUS "profile: ${_profile}, build id: ${PHEROES_BUILD_ID}")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -Wextra -O2 -std=gnu17")
file(GLOB _v2d_shaders lib/vita2d/shader/*_gxp.o)
add_library(vita2d STATIC lib/vita2d/source/vita2d.c lib/vita2d/source/vita2d_texture.c
            lib/vita2d/source/vita2d_draw.c lib/vita2d/source/utils.c ${_v2d_shaders})
set_source_files_properties(${_v2d_shaders} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
target_include_directories(vita2d PUBLIC lib/vita2d/include)
target_compile_options(vita2d PRIVATE -w)

file(GLOB _gxps ${CMAKE_CURRENT_SOURCE_DIR}/lib/vitashaders/*.gxp)
set_source_files_properties(lib/vitashaders/shaders.S PROPERTIES
  COMPILE_OPTIONS "-Wa,-I${CMAKE_CURRENT_SOURCE_DIR}/lib/vitashaders"
  OBJECT_DEPENDS "${_gxps}")

add_executable(palmheroes
    main.c
    natives.c
    win.c
    music.c
    natives.S
    lib/vitashaders/shaders.S
    lib/vshot/vshot.c
)
target_compile_definitions(palmheroes PRIVATE PHEROES_BUILD_ID="${PHEROES_BUILD_ID} ${_profile}")
if(DIAGNOSTICS)
  target_compile_definitions(palmheroes PRIVATE DIAGNOSTICS)
  target_sources(palmheroes PRIVATE lib/vshot/vshot.c)
endif()
target_link_options(palmheroes PRIVATE -Wl,--wrap=_exit)
target_link_libraries(palmheroes m vita2d SceGxm_stub SceAppMgr_stub SceCommonDialog_stub SceSysmodule_stub ScePower_stub kubridge_stub SceSysmem_stub SceIofilemgr_stub SceProcessmgr_stub SceLibKernel_stub SceKernelThreadMgr_stub SceDisplay_stub SceTouch_stub SceCtrl_stub SceAudio_stub mpg123 m)
vita_create_self(eboot.bin palmheroes UNSAFE)

if(NOT EXISTS ${GAME_DIR}/Data/game.gfx OR NOT EXISTS ${GAME_DIR}/ph.img)
  message(FATAL_ERROR "no game in ${GAME_DIR}: run tools/prepare_game.py PalmHeroes1.05.3.zip game first")
endif()
set(_la ${CMAKE_CURRENT_SOURCE_DIR}/livearea)
vita_create_vpk(palmheroes.vpk ${VITA_TITLEID} eboot.bin VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
                FILE ${GAME_DIR}/ph.img ph.img ${GAME_DIR}/Data game/Data ${GAME_DIR}/Maps game/Maps
                ${_la}/icon0.png sce_sys/icon0.png
                ${_la}/pic0.png sce_sys/pic0.png
                ${_la}/bg0.png sce_sys/livearea/contents/bg0.png
                ${_la}/startup.png sce_sys/livearea/contents/startup.png
                ${_la}/template.xml sce_sys/livearea/contents/template.xml)
