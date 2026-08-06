cmake_minimum_required(VERSION 4.0.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_CXX_EXTENSIONS OFF)
project(DJIC)

include(FetchContent)
include(CTest)

find_package(argparse CONFIG REQUIRED)
find_package(ut CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS system)
find_package(simdjson CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE 
    spdlog::spdlog argparse::argparse 
    PkgConfig::LIBUSB Boost::system simdjson::simdjson
)
target_sources(${PROJECT_NAME} PUBLIC FILE_SET CXX_MODULES FILES
    src/at.cppm src/esim.cppm src/sms.cppm
    src/modem.cppm src/pdu.cppm src/asterisk.cppm
)

if(BUILD_TESTING)
    add_executable(DJIC_tests tests/main.cpp)
    target_compile_options(DJIC_tests PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    target_sources(DJIC_tests PRIVATE FILE_SET CXX_MODULES FILES
        src/at.cppm src/esim.cppm src/sms.cppm
        src/modem.cppm src/pdu.cppm src/asterisk.cppm
        tests/support.cppm tests/at.cppm tests/pdu.cppm
        tests/sms.cppm tests/modem.cppm
    )
    target_link_libraries(DJIC_tests PRIVATE
        spdlog::spdlog argparse::argparse
        PkgConfig::LIBUSB Boost::system simdjson::simdjson Boost::ut
    )
    add_test(NAME DJIC_at_tests COMMAND DJIC_tests "AT parsing")
    add_test(NAME DJIC_pdu_tests COMMAND DJIC_tests "PDU codec")
    add_test(NAME DJIC_sms_tests COMMAND DJIC_tests "SMS logic")
    add_test(NAME DJIC_modem_tests COMMAND DJIC_tests "Modem and Asterisk state")
endif()
