cmake_minimum_required(VERSION 3.16)
project(ethercat-weigh VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(EW_BUILD_TESTS "Unit-Tests bauen" ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
foreach(hdr SOEM/soem/ethercat.h json/single_include/nlohmann/json.hpp
            Crow/include/crow.h asio/asio/include/asio.hpp)
  if(NOT EXISTS ${TP}/${hdr})
    message(FATAL_ERROR "third_party/${hdr} fehlt: git submodule update --init")
  endif()
endforeach()

find_package(Threads REQUIRED)

# --- SOEM (direkt aus den Quellen, ohne dessen CMake) -------------------------
file(GLOB SOEM_SOURCES ${TP}/SOEM/soem/*.c ${TP}/SOEM/osal/linux/*.c ${TP}/SOEM/oshw/linux/*.c)
add_library(soem STATIC ${SOEM_SOURCES})
target_include_directories(soem PUBLIC
  ${TP}/SOEM/soem ${TP}/SOEM/osal ${TP}/SOEM/osal/linux ${TP}/SOEM/oshw/linux)
target_compile_options(soem PRIVATE -w)
target_link_libraries(soem PUBLIC Threads::Threads)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  target_link_libraries(soem PUBLIC rt)
endif()

# --- Header-only Abhängigkeiten ----------------------------------------------
add_library(ew_deps INTERFACE)
target_include_directories(ew_deps SYSTEM INTERFACE
  ${TP}/json/single_include
  ${TP}/Crow/include
  ${TP}/asio/asio/include)
target_compile_definitions(ew_deps INTERFACE ASIO_STANDALONE)

# --- Kernlogik (hardwareunabhängig, testbar) ----------------------------------
add_library(ew_core STATIC
  src/calibration.cpp
  src/calibration_controller.cpp
  src/config.cpp
  src/el3356.cpp
  src/measurement.cpp
  src/statistics.cpp
  src/sim_backend.cpp)
target_include_directories(ew_core PUBLIC src)
target_link_libraries(ew_core PUBLIC ew_deps)
target_compile_options(ew_core PRIVATE -Wall -Wextra)

# --- Anwendung ---------------------------------------------------------------
add_executable(ethercat-weigh
  src/main.cpp
  src/ethercat_backend.cpp
  src/acquisition.cpp
  src/web_server.cpp)
target_link_libraries(ethercat-weigh PRIVATE ew_core soem Threads::Threads)
target_compile_options(ethercat-weigh PRIVATE -Wall -Wextra)
target_compile_definitions(ethercat-weigh PRIVATE EW_VERSION="${PROJECT_VERSION}")

install(TARGETS ethercat-weigh RUNTIME DESTINATION bin)

if(EW_BUILD_TESTS)
  enable_testing()
  add_executable(ew_tests tests/test_main.cpp)
  target_link_libraries(ew_tests PRIVATE ew_core)
  add_test(NAME ew_tests COMMAND ew_tests)
endif()
