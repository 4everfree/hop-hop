cmake_minimum_required(VERSION 3.21)
project(hop-hop VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# --- toml++ -----------------------------------------------------------------
# Prefer a system package; otherwise drop the pinned single header into the
# build tree. Keeps the repo free of a 500 KB vendored blob.
find_package(tomlplusplus QUIET)
if(NOT tomlplusplus_FOUND)
    set(TOMLPP_VERSION v3.4.0)
    set(TOMLPP_DIR ${CMAKE_BINARY_DIR}/_deps/tomlplusplus)
    if(NOT EXISTS ${TOMLPP_DIR}/toml.hpp)
        message(STATUS "Fetching toml++ ${TOMLPP_VERSION} single header")
        file(DOWNLOAD
            https://raw.githubusercontent.com/marzer/tomlplusplus/${TOMLPP_VERSION}/toml.hpp
            ${TOMLPP_DIR}/toml.hpp
            TLS_VERIFY ON
            STATUS TOMLPP_DL)
        list(GET TOMLPP_DL 0 TOMLPP_DL_CODE)
        if(NOT TOMLPP_DL_CODE EQUAL 0)
            message(FATAL_ERROR "Could not fetch toml++: ${TOMLPP_DL}. "
                                "Install the tomlplusplus package instead.")
        endif()
    endif()
    add_library(tomlplusplus_tomlplusplus INTERFACE)
    target_include_directories(tomlplusplus_tomlplusplus INTERFACE ${TOMLPP_DIR})
    add_library(tomlplusplus::tomlplusplus ALIAS tomlplusplus_tomlplusplus)
endif()

# --- sources ----------------------------------------------------------------
set(SOURCES
    resources.qrc
    src/main.cpp
    src/config.cpp
    src/detect.cpp
    src/single_instance.cpp
    src/main_window.cpp
)

if(APPLE)
    enable_language(OBJCXX)
    list(APPEND SOURCES src/platform_macos.mm)
elseif(WIN32)
    list(APPEND SOURCES src/platform_windows.cpp)
else()
    list(APPEND SOURCES src/platform_linux.cpp)
endif()

add_executable(hop-hop ${SOURCES})
target_link_libraries(hop-hop PRIVATE Qt6::Widgets tomlplusplus::tomlplusplus)

if(APPLE)
    find_library(APPKIT_LIBRARY AppKit REQUIRED)
    target_link_libraries(hop-hop PRIVATE ${APPKIT_LIBRARY})
    set_target_properties(hop-hop PROPERTIES MACOSX_BUNDLE FALSE)
elseif(WIN32)
    # WMI for command lines, psapi for memory, shell32 for CommandLineToArgvW
    target_link_libraries(hop-hop PRIVATE wbemuuid ole32 oleaut32 psapi shell32)
    set_target_properties(hop-hop PROPERTIES WIN32_EXECUTABLE TRUE)
else()
    # window activation goes through KWin scripting over D-Bus
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    target_link_libraries(hop-hop PRIVATE Qt6::DBus)
endif()

install(TARGETS hop-hop RUNTIME DESTINATION bin)
