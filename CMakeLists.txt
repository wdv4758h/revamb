#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

cmake_minimum_required(VERSION 2.8)

project(revamb)

# Doxygen
find_package(Doxygen)
if(DOXYGEN_FOUND)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in
    ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile @ONLY)
  add_custom_target(doc
    ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating API documentation with Doxygen" VERBATIM)
endif(DOXYGEN_FOUND)

# LLVM CMake stuff
find_package(LLVM REQUIRED CONFIG)
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(LLVM_LIBRARIES core support irreader ScalarOpts
  linker Analysis object transformutils)

set(QEMU_INSTALL_PATH "/usr" CACHE PATH "Path to the QEMU installation.")
add_definitions("-DQEMU_INSTALL_PATH=\"${QEMU_INSTALL_PATH}\"")
add_definitions("-DINSTALL_PATH=\"${CMAKE_INSTALL_PREFIX}\"")
include_directories("${QEMU_INSTALL_PATH}/include/")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Werror -Wno-error=unused-variable")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=return-type")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=unused-function")
if(CMAKE_CXX_COMPILER_ID EQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=unused-but-set-variable")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=maybe-uninitialized")
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
add_definitions("-D_FILE_OFFSET_BITS=64")
include_directories(argparse/)

set(CMAKE_INSTALL_RPATH "\$ORIGIN/../lib${LLVM_LIBDIR_SUFFIX}")

add_executable(revamb ptcdump.cpp main.cpp debughelper.cpp variablemanager.cpp
  jumptargetmanager.cpp instructiontranslator.cpp codegenerator.cpp
  debug.cpp osra.cpp set.cpp simplifycomparisons.cpp reachingdefinitions.cpp
  functionboundariesdetection.cpp noreturnanalysis.cpp argparse/argparse.c)
target_link_libraries(revamb dl m ${LLVM_LIBRARIES})
install(TARGETS revamb RUNTIME DESTINATION bin)

configure_file(li-csv-to-ld-options "${CMAKE_BINARY_DIR}/li-csv-to-ld-options"
  COPYONLY)
configure_file(support.c "${CMAKE_BINARY_DIR}/support.c" COPYONLY)
configure_file(translate "${CMAKE_BINARY_DIR}/translate" COPYONLY)
install(PROGRAMS translate li-csv-to-ld-options DESTINATION bin)
install(FILES support.c DESTINATION share/revamb)

# Remove -rdynamic
set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS)

include(tests/Tests.cmake)
