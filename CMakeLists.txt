cmake_minimum_required(VERSION 3.21)
project(rodip VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets DBus)
find_package(X11)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

add_executable(rodip src/main.cpp src/waylandcapture.cpp src/waylandcapture.h)
target_link_libraries(rodip PRIVATE Qt6::Widgets Qt6::DBus PkgConfig::PIPEWIRE)
if(X11_FOUND)
    target_link_libraries(rodip PRIVATE X11::X11)
    target_compile_definitions(rodip PRIVATE RODIP_X11=1)
endif()

install(TARGETS rodip RUNTIME DESTINATION bin)
