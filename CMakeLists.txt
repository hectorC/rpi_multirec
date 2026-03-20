cmake_minimum_required(VERSION 3.16)
project(rpi_multirec CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(SNDFILE REQUIRED sndfile)
pkg_check_modules(LIBGPIOD REQUIRED IMPORTED_TARGET libgpiod)

add_executable(rpi_multirec
    src/app.cpp
    src/main.cpp
)

target_include_directories(rpi_multirec PRIVATE
    ${ALSA_INCLUDE_DIRS}
    ${SNDFILE_INCLUDE_DIRS}
    ${LIBGPIOD_INCLUDE_DIRS}
)

target_link_libraries(rpi_multirec PRIVATE
    ${ALSA_LIBRARIES}
    ${SNDFILE_LIBRARIES}
    PkgConfig::LIBGPIOD
)

target_compile_options(rpi_multirec PRIVATE -Wall -Wextra -Wpedantic)
