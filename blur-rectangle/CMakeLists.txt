cmake_minimum_required(VERSION 3.20)

project(blur-rectangle VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

find_package(Qt6 REQUIRED COMPONENTS DBus Widgets)
find_package(KF6 REQUIRED COMPONENTS WindowSystem)
find_package(OpenCV REQUIRED COMPONENTS core imgproc video tracking)

add_executable(blur-rectangle
    src/main.cpp
    src/blurwindow.cpp
    src/blurwindow.h
    src/blurwindow_constants.h
    src/blurwindow_menu.cpp
    src/blurwindow_tracking.cpp
    src/framesource.cpp
    src/framesource.h
    src/matcher.cpp
    src/matcher.h
    src/objecttracker.cpp
    src/objecttracker.h
    src/tracker.h
)

target_link_libraries(blur-rectangle
    PRIVATE
        Qt6::Widgets
        Qt6::DBus
        KF6::WindowSystem
        ${OpenCV_LIBS}
)

target_include_directories(blur-rectangle SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})

enable_testing()
add_executable(matcher-test tests/matcher_test.cpp src/matcher.cpp src/matcher.h)
target_link_libraries(matcher-test PRIVATE ${OpenCV_LIBS})
target_include_directories(matcher-test SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
target_include_directories(matcher-test PRIVATE src)
add_test(NAME matcher COMMAND matcher-test)

install(TARGETS blur-rectangle DESTINATION bin)
install(FILES packaging/org.local.blurrectangle.desktop DESTINATION share/applications)
