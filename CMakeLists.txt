cmake_minimum_required(VERSION 3.19)
project(WsprLive VERSION 0.2.1 LANGUAGES CXX)   # WSPR Scheduler Live (C# WSPR_Live was 0.1.20)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Sql Network)

# Non-window part (wspr.live, the reported table, DBcredential, ~/LiveCall) - shared by the program and tests
add_library(livecore STATIC
    livecore.h livecore.cpp
    credentials.h credentials.cpp
)
target_link_libraries(livecore PUBLIC Qt6::Core Qt6::Sql Qt6::Network)
target_include_directories(livecore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set(WSPRLIVE_SOURCES liveform.h liveform.cpp liveform.ui livetablemodel.h livetablemodel.cpp msg.h msg.cpp
    loaderror.h loaderror.cpp)

if(WIN32)   # icon and version details for WsprLive.exe (Explorer, taskbar, installer)
    set(RC_ICON "${CMAKE_CURRENT_SOURCE_DIR}/wsprlive.ico")
    set(RC_NAME "WSPR Scheduler Live")
    set(RC_EXE WsprLive)
    configure_file(wsprlive.rc.in "${CMAKE_CURRENT_BINARY_DIR}/wsprlive.rc" @ONLY)
    set(WSPRLIVE_RC "${CMAKE_CURRENT_BINARY_DIR}/wsprlive.rc")
endif()

add_executable(WsprLive WIN32 livemain.cpp ${WSPRLIVE_SOURCES} ${WSPRLIVE_RC})
target_link_libraries(WsprLive PRIVATE livecore Qt6::Widgets)
target_compile_definitions(WsprLive PRIVATE WSPRLIVE_VERSION="${PROJECT_VERSION}")

# Windows: copy the Qt DLLs and plugins next to the program after each build (windeployqt).
# The MySQL driver (sqldrivers/qsqlmysql.dll) and libmysql.dll are not part of Qt - copy those yourself.
if(WIN32)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT6_INSTALL_PREFIX}/bin" "${Qt6_DIR}/../../../bin")
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET WsprLive POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}" --no-translations --verbose 0 "$<TARGET_FILE:WsprLive>"
            COMMENT "windeployqt WsprLive")
    else()
        message(WARNING "windeployqt not found - run the program from Qt Creator or add the Qt bin folder to PATH")
    endif()
endif()

option(WSPRLIVE_TESTS "Build the tests (the database ones need a scratch MySQL, never the real one)" OFF)
if(WSPRLIVE_TESTS)
    add_executable(live_test tests/live_test.cpp)   # core: fake wspr.live; scratch MySQL optional
    target_link_libraries(live_test PRIVATE livecore)
    add_executable(liveform_uitest tests/liveform_uitest.cpp ${WSPRLIVE_SOURCES})   # the window; offscreen
    target_link_libraries(liveform_uitest PRIVATE livecore Qt6::Widgets)
    target_compile_definitions(liveform_uitest PRIVATE WSPRLIVE_VERSION="${PROJECT_VERSION}")
endif()
