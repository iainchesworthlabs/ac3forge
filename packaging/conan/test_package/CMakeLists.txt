cmake_minimum_required(VERSION 3.28)
project(test_ac3forge CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ac3forge CONFIG REQUIRED)

# {fmt} here is this test's OWN choice of output (fmt::println), unrelated to
# ac3forge's own private use of it - see the main recipe's requirements() for
# why ac3forge itself never needs a consumer to resolve fmt.
find_package(fmt CONFIG REQUIRED)

add_executable(test_ac3forge src/test_ac3forge.cpp)
target_link_libraries(test_ac3forge PRIVATE ac3::forge fmt::fmt)
