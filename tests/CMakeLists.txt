#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

cmake_minimum_required(VERSION 2.8)

project(Test C)

string(REPLACE ":" ";" TESTS "${TESTS}")

foreach(TEST_NAME ${TESTS})
  string(REPLACE ":" ";" TEST_SOURCES_${TEST_NAME} "${TEST_SOURCES_${TEST_NAME}}")
  add_executable(${TEST_NAME} ${TEST_SOURCES_${TEST_NAME}})
  target_link_libraries(${TEST_NAME} "${LINK_LIBRARIES}")
  install(TARGETS ${TEST_NAME} RUNTIME DESTINATION bin)
endforeach()
