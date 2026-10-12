cmake_minimum_required(VERSION 3.20)

project(kwin_tiny_blur_effect VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
if(NOT KDE_INSTALL_QTPLUGINDIR)
    set(KDE_INSTALL_QTPLUGINDIR "${KDE_INSTALL_LIBDIR}/qt6/plugins")
endif()
set(KDE_INSTALL_PLUGINDIR "${KDE_INSTALL_QTPLUGINDIR}")

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets DBus)
find_package(KF6 REQUIRED COMPONENTS CoreAddons Config WindowSystem)
find_package(KWin REQUIRED)
find_package(epoxy REQUIRED)

find_path(KWIN_PRIVATE_INCLUDE_DIR
    NAMES effect/effect.h
    PATHS
        "${KDE_INSTALL_INCLUDEDIR}/kwin"
        "/usr/include/kwin"
        "/usr/local/include/kwin"
)
if(NOT KWIN_PRIVATE_INCLUDE_DIR)
    message(FATAL_ERROR "Could not find KWin private headers. Install KWin development headers or pass -DKWIN_PRIVATE_INCLUDE_DIR=/path/to/include/kwin")
endif()

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

kcoreaddons_add_plugin(stableblur INSTALL_NAMESPACE "kwin/effects/plugins" SOURCES
    tinyblur.cpp
    tinyblur.qrc
    main.cpp
)

target_link_libraries(stableblur PRIVATE
    kwin
    Qt6::Widgets
    Qt6::DBus
    KF6::CoreAddons
    KF6::ConfigGui
    KF6::WindowSystem
    epoxy::epoxy
)

target_include_directories(stableblur PRIVATE "${KWIN_PRIVATE_INCLUDE_DIR}")

install(FILES metadata.json DESTINATION ${KDE_INSTALL_DATADIR}/kwin-wayland/builtin-effects/ RENAME stableblur.json)
