cmake_minimum_required(VERSION 3.16)
project(multicore_ipc_sim LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SIM_WERROR "Promote warnings to errors" OFF)
option(SIM_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# The DSP end reuses the algorithm library from the mini_dsp checkout; point
# this elsewhere if the two projects are not siblings on disk.
set(MINI_DSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mini_dsp" CACHE PATH "mini_dsp checkout")

set(SIM_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes)

function(sim_harden target)
    target_compile_options(${target} PRIVATE ${SIM_WARNINGS})
    if(SIM_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(SIM_SANITIZE)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

add_library(mcshm STATIC src/mcshm_ring.c)
target_include_directories(mcshm PUBLIC include)
sim_harden(mcshm)

add_library(mccmd STATIC src/mccmd.c)
target_include_directories(mccmd PUBLIC include)
sim_harden(mccmd)

add_library(mcpcm STATIC src/mcpcm.c)
target_include_directories(mcpcm PUBLIC include)
target_link_libraries(mcpcm PUBLIC mcshm mccmd)
sim_harden(mcpcm)

add_library(mcpcm_ops STATIC src/mcpcm_ops.c)
target_include_directories(mcpcm_ops PUBLIC include)
target_link_libraries(mcpcm_ops PUBLIC mcpcm)
sim_harden(mcpcm_ops)

add_library(mcs16 STATIC src/mcs16.c)
target_include_directories(mcs16 PUBLIC include)
sim_harden(mcs16)

# The ALSA backend is optional: without libasound the project still builds and
# the simulated backend keeps the whole test suite meaningful.
find_library(ASOUND_LIBRARY NAMES asound)
if(ASOUND_LIBRARY)
    target_sources(mcpcm_ops PRIVATE src/mcpcm_alsa.c)
    target_link_libraries(mcpcm_ops PUBLIC ${ASOUND_LIBRARY})
    target_compile_definitions(mcpcm_ops PUBLIC HAVE_ALSA=1)
    message(STATUS "ALSA backend enabled (${ASOUND_LIBRARY})")
else()
    message(STATUS "libasound not found: only the shm backend is available")
endif()

if(EXISTS "${MINI_DSP_DIR}/framework/dsp_pipeline.c")
    add_library(minidsp_algos STATIC
        ${MINI_DSP_DIR}/framework/dsp_pipeline.c
        ${MINI_DSP_DIR}/framework/dsp_error.c
        ${MINI_DSP_DIR}/framework/dsp_block.c
        ${MINI_DSP_DIR}/framework/fft.c
        ${MINI_DSP_DIR}/module/biquad.c
        ${MINI_DSP_DIR}/module/agc.c
        ${MINI_DSP_DIR}/module/ns.c)
    target_include_directories(minidsp_algos PUBLIC
        ${MINI_DSP_DIR}/framework
        ${MINI_DSP_DIR}/module
        ${MINI_DSP_DIR}/audio)
    sim_harden(minidsp_algos)
    set(HAVE_MINIDSP TRUE)
else()
    message(WARNING "mini_dsp not found at ${MINI_DSP_DIR}; the DSP end will not be built")
    set(HAVE_MINIDSP FALSE)
endif()

add_executable(ring_selftest tests/ring_selftest.c)
target_link_libraries(ring_selftest PRIVATE mcshm rt)
sim_harden(ring_selftest)

add_executable(pvm_router apps/pvm_router.c)
target_link_libraries(pvm_router PRIVATE mcshm mccmd)
sim_harden(pvm_router)

add_executable(gvm_app apps/gvm_app.c)
target_link_libraries(gvm_app PRIVATE mcpcm_ops mcshm mccmd m)
sim_harden(gvm_app)

enable_testing()
add_test(NAME ring_selftest COMMAND ring_selftest)
add_test(NAME ipc_single_hop
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_m2.sh
                 $<TARGET_FILE:pvm_router> $<TARGET_FILE:gvm_app>)
set_tests_properties(ipc_single_hop PROPERTIES TIMEOUT 60)

add_test(NAME pcm_state_machine
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_m3.sh
                 $<TARGET_FILE:pvm_router> $<TARGET_FILE:gvm_app>)
set_tests_properties(pcm_state_machine PROPERTIES TIMEOUT 90)

if(HAVE_MINIDSP)
    add_executable(dsp_engine apps/dsp_engine.c)
    target_link_libraries(dsp_engine PRIVATE mcs16 mccmd mcshm minidsp_algos m)
    sim_harden(dsp_engine)

    add_executable(multichannel_selftest tests/multichannel_selftest.c)
    target_link_libraries(multichannel_selftest PRIVATE minidsp_algos m)
    sim_harden(multichannel_selftest)
    add_test(NAME multichannel_32 COMMAND multichannel_selftest)

    add_test(NAME three_process_pipeline
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_m4.sh
                     $<TARGET_FILE:pvm_router> $<TARGET_FILE:gvm_app> $<TARGET_FILE:dsp_engine>)
    set_tests_properties(three_process_pipeline PROPERTIES TIMEOUT 90)

    add_test(NAME fault_injection
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_m5.sh
                     $<TARGET_FILE:pvm_router> $<TARGET_FILE:gvm_app> $<TARGET_FILE:dsp_engine>)
    set_tests_properties(fault_injection PROPERTIES TIMEOUT 90)

    add_test(NAME channel_parameterisation
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_m7.sh
                     $<TARGET_FILE:pvm_router> $<TARGET_FILE:gvm_app> $<TARGET_FILE:dsp_engine>)
    set_tests_properties(channel_parameterisation PROPERTIES TIMEOUT 120)

    add_test(NAME direct_topology
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_m8.sh
                     $<TARGET_FILE:pvm_router> $<TARGET_FILE:gvm_app> $<TARGET_FILE:dsp_engine>)
    set_tests_properties(direct_topology PROPERTIES TIMEOUT 90)
endif()

if(ASOUND_LIBRARY)
    add_test(NAME alsa_backend
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_m6.sh $<TARGET_FILE:gvm_app>)
    set_tests_properties(alsa_backend PROPERTIES TIMEOUT 60)
endif()
