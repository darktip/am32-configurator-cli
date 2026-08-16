cmake_minimum_required(VERSION 3.20)

project(am32-cli
    VERSION 0.1.0
    LANGUAGES CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "am32-cli currently implements serial I/O with the Windows API and only builds on Windows.")
endif()

add_executable(am32-cli
    main.cpp
)

target_compile_features(am32-cli PRIVATE cxx_std_17)

target_compile_definitions(am32-cli PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(am32-cli PRIVATE /W4 /permissive-)
else()
    target_compile_options(am32-cli PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS am32-cli
    RUNTIME DESTINATION bin
)
