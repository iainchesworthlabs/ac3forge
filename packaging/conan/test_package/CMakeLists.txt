cmake_minimum_required(VERSION 3.28)
project(test_iclforge CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(iclforge CONFIG REQUIRED)

# {fmt} here is this test's OWN choice of output (fmt::println), unrelated to
# iclforge's own private use of it - see the main recipe's requirements() for
# why iclforge itself never needs a consumer to resolve fmt.
find_package(fmt CONFIG REQUIRED)

add_executable(test_iclforge src/test_iclforge.cpp)
target_link_libraries(test_iclforge PRIVATE iclforge::ac3 fmt::fmt)
