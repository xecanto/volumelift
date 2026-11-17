cmake_minimum_required(VERSION 3.21)
project(VolumeLift VERSION 0.1.1 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "VolumeLift builds for 64-bit Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the APO loads into audiodg.exe, which must not depend on a VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
endif()

add_library(VolumeLiftApo SHARED
    src/apo/ApoLog.cpp
    src/apo/dllmain.cpp
    src/apo/SettingsWatcher.cpp
    src/apo/StatusSection.cpp
    src/apo/VolumeLiftApo.cpp
    src/apo/VolumeLiftApo.def)
target_include_directories(VolumeLiftApo PRIVATE src)
target_link_libraries(VolumeLiftApo PRIVATE advapi32 ole32)

add_executable(VolumeLift WIN32
    src/app/main.cpp
    src/app/Installer.cpp
    src/app/VolumeLift.rc
    src/app/VolumeLift.manifest)
target_include_directories(VolumeLift PRIVATE src)
target_link_libraries(VolumeLift PRIVATE comctl32 shell32 ole32 advapi32 user32 gdi32)

enable_testing()
add_executable(booster_test tests/booster_test.cpp)
target_include_directories(booster_test PRIVATE src)
add_test(NAME booster COMMAND booster_test)
