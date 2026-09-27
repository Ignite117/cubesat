cmake_minimum_required(VERSION 3.16)

project(cubesat VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets LinguistTools)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets LinguistTools)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Charts Network)
set(TS_FILES cubesat_ru_RU.ts)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
        protocol.h
        udplink.cpp
        udplink.h
        rawlog.cpp
        rawlog.h
        limitcheck.cpp
        limitcheck.h
        ${TS_FILES}
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(cubesat
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
        telemetry.h
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET cubesat APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation

    qt_create_translation(QM_FILES ${CMAKE_SOURCE_DIR} ${TS_FILES})
else()
    if(ANDROID)
        add_library(cubesat SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(cubesat
            ${PROJECT_SOURCES}
        )
    endif()

    qt5_create_translation(QM_FILES ${CMAKE_SOURCE_DIR} ${TS_FILES})
endif()

target_link_libraries(cubesat PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Charts
    Qt${QT_VERSION_MAJOR}::Network
)

# Иконка окна берётся из каталога с исполняемым файлом, а не по абсолютному пути.
add_custom_command(TARGET cubesat POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/cub3.png"
            "$<TARGET_FILE_DIR:cubesat>/cub3.png"
)

# Тесты протокола и границ нормы. Только QtCore: проверяются чистые функции,
# без окон. Запуск: ctest --test-dir <каталог сборки> --output-on-failure
option(CUBESAT_BUILD_TESTS "Собирать тесты" ON)
if(CUBESAT_BUILD_TESTS)
    enable_testing()
    add_executable(cubesat_tests
        tests/test_main.cpp
        limitcheck.cpp
        limitcheck.h
        protocol.h
        telemetry.h
    )
    set_target_properties(cubesat_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_include_directories(cubesat_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cubesat_tests PRIVATE Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME cubesat_tests COMMAND cubesat_tests)
endif()

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.cubesat)
endif()
set_target_properties(cubesat PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS cubesat
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(cubesat)
endif()
