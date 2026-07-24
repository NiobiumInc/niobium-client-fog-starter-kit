# Copyright 2026 Niobium Microsystems, Inc.
# Licensed under the Apache License, Version 2.0.

cmake_minimum_required(VERSION 3.18)
project(niobium-client-fog-dotprod LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── One OpenFHE only: the niobium-client's ────────────────────────────────────
# This kit builds *exclusively* against the OpenFHE the niobium-client vendors
# and installs (the Niobium-instrumented 1.4.2 fork at <client>/vendor/lib/openfhe).
# There is intentionally no "point at any system OpenFHE" path — a second OpenFHE
# would be a different version/flavor than what actually records and runs.
# Always drive the build through scripts/build_task.sh, which builds the client
# and sets NIOBIUM_SDK_BUILD=ON + CMAKE_PREFIX_PATH to the client's OpenFHE.
option(NIOBIUM_SDK_BUILD "Build against the niobium-client's OpenFHE (required)" OFF)
if(NOT NIOBIUM_SDK_BUILD)
  message(FATAL_ERROR
    "This kit builds only against the niobium-client's OpenFHE.\n"
    "Run scripts/build_task.sh (it builds the client and passes "
    "-DNIOBIUM_SDK_BUILD=ON -DCMAKE_PREFIX_PATH=<client>/vendor/lib/openfhe).\n"
    "See docs/USING_THE_CLIENT.md.")
endif()

# Client location: -DNB_CLIENT_DIR=..., else $NIOBIUM_CLIENT_DIR (standalone
# clone), else the vendored submodule. See docs/USING_THE_CLIENT.md.
if(NOT NB_CLIENT_DIR)
  if(DEFINED ENV{NIOBIUM_CLIENT_DIR})
    set(NB_CLIENT_DIR "$ENV{NIOBIUM_CLIENT_DIR}")
  else()
    set(NB_CLIENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/niobium-client")
  endif()
endif()
message(STATUS "niobium-client at ${NB_CLIENT_DIR}")
set(NB_FHETCH_DIR "${NB_CLIENT_DIR}/vendor/niobium-fhetch")

find_package(OpenFHE CONFIG REQUIRED)   # from CMAKE_PREFIX_PATH = client's vendor/lib/openfhe
message(STATUS "OpenFHE (client) ${OpenFHE_INCLUDE}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenFHE_CXX_FLAGS} -Wno-error")

include_directories(
  ${OpenFHE_INCLUDE} ${OpenFHE_INCLUDE}/core ${OpenFHE_INCLUDE}/pke ${OpenFHE_INCLUDE}/binfhe
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${NB_FHETCH_DIR}/include                        # niobium/compiler.h (SDK)
  ${NB_FHETCH_DIR}/vendor/json/single_include)    # nlohmann/json (if vendored there)
link_directories(${OpenFHE_LIBDIR})

find_library(NBFHETCH_LIB NAMES nbfhetch
  PATHS "${NB_CLIENT_DIR}/build/vendor/niobium-fhetch"
        "${NB_CLIENT_DIR}/build/_deps/niobium-fhetch-build" NO_DEFAULT_PATH)
if(NOT NBFHETCH_LIB)
  message(FATAL_ERROR "libnbfhetch not found — run 'make -C ${NB_CLIENT_DIR} release' first")
endif()
message(STATUS "nbfhetch ${NBFHETCH_LIB}")
get_filename_component(NBFHETCH_DIR "${NBFHETCH_LIB}" DIRECTORY)
set(CMAKE_BUILD_RPATH "${OpenFHE_LIBDIR};${NBFHETCH_DIR}")

# zlib is OPTIONAL: the client's bundled OpenFHE .so already pulls libz at runtime
# (DT_NEEDED). Link it only if the dev headers are present — some Fog hosts have
# runtime libz but not -dev, so find_package(ZLIB REQUIRED) would wrongly fail.
find_package(ZLIB QUIET)
set(SDK_ZLIB "")
if(ZLIB_FOUND)
  set(SDK_ZLIB ZLIB::ZLIB)
endif()

# libnbfhetch defers its OpenFHE (lbcrypto) symbols to the final link; keep
# OpenFHE in DT_NEEDED so the default --as-needed can't drop it (else
# dp_compute_sdk fails with "undefined reference to lbcrypto::...").
if(UNIX AND NOT APPLE)
  add_link_options(-Wl,--no-as-needed -Wl,--disable-new-dtags)
endif()

# Client stages: plain OpenFHE; link nbfhetch to resolve the instrumented-OpenFHE
# globals (they never call the recording API themselves).
foreach(stage dp_keygen dp_encrypt dp_decrypt)
  add_executable(${stage} src/${stage}.cpp src/dotprod.cpp)
  target_link_libraries(${stage} ${OpenFHE_SHARED_LIBRARIES} ${NBFHETCH_LIB} ${SDK_ZLIB})
endforeach()

# SDK-linked compute stage: compile/run via libnbfhetch → the FHETCH transport.
# OPENFHE_CPROBES is REQUIRED so the kernel's ct*ct EvalMult fires the SDK's
# compile-time probes (else 0 muls captured — the historical "missing-MUL" bug).
# On the first run it also computes the CPU result, so a no-server run CPU-verifies
# the math locally (this is the kit's "validate locally" path).
add_executable(dp_compute_sdk src/dp_compute_sdk.cpp src/dotprod.cpp)
target_compile_definitions(dp_compute_sdk PRIVATE NIOBIUM_COMPILER OPENFHE_CPROBES)
target_link_libraries(dp_compute_sdk ${OpenFHE_SHARED_LIBRARIES} ${NBFHETCH_LIB} ${SDK_ZLIB})
