cmake_minimum_required(VERSION 3.16)

set(n ai-quiz-gen)
project(${n})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(ZLIB REQUIRED)

add_executable(${n} ${n}.cpp gift-validator/gift-validator.cpp)

target_link_libraries(${n} PRIVATE CURL::libcurl nlohmann_json::nlohmann_json ZLIB::ZLIB)
