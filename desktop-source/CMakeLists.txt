find_package(SQLite3 REQUIRED)
cmake_minimum_required(VERSION 3.20)
project(pdfcsv VERSION 0.10.6 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(pdfcsv
    src/main.cpp
    src/terminology.cpp
    src/input_formats.cpp

    src/anki_apkg.cpp)

target_link_libraries(pdfcsv PRIVATE Threads::Threads)

if (MSVC)
    target_compile_options(pdfcsv PRIVATE /W4 /permissive-)
else()
    target_compile_options(pdfcsv PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS pdfcsv RUNTIME DESTINATION bin)

target_link_libraries(pdfcsv PRIVATE SQLite::SQLite3)

