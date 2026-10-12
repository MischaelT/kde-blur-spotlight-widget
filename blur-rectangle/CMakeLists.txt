cmake_minimum_required(VERSION 3.20)

project(blur-rectangle VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

find_package(Qt6 REQUIRED COMPONENTS DBus Widgets)
find_package(KF6 REQUIRED COMPONENTS WindowSystem)

add_executable(blur-rectangle
    src/main.cpp
    src/blurwindow.cpp
    src/blurwindow.h
)

target_link_libraries(blur-rectangle
    PRIVATE
        Qt6::Widgets
        Qt6::DBus
        KF6::WindowSystem
)

install(TARGETS blur-rectangle DESTINATION bin)
install(FILES packaging/org.local.blurrectangle.desktop DESTINATION share/applications)
