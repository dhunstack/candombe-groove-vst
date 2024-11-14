cmake_minimum_required(VERSION 3.16)
project(JUCECMakeRepo)

# Common setup
include(CMake/CPM.cmake)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
set_property(GLOBAL PROPERTY USE_FOLDERS YES)
option(JUCE_ENABLE_MODULE_SOURCE_GROUPS "Enable Module Source Groups" ON)
option(JUCE_BUILD_EXTRAS "Build JUCE Extras" OFF)
option(JUCE_BUILD_EXAMPLES "Build JUCE Examples" OFF)
option(BUILD_JUCE_PROTOTYPE_EXAMPLES "Build JUCE prototype examples" ON)
option(BUILD_UNIT_TESTS "Build JUCE prototype examples" ON)

# Platform-specific settings
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.14" CACHE STRING "Minimum OS X deployment version" FORCE)
    option(UniversalBinary "Build universal binary for mac" OFF)
    if (UniversalBinary)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE INTERNAL "")
    endif()

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE INTERNAL "")
    else()
        set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE INTERNAL "")
    endif()


elseif(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(UNIX AND NOT APPLE) # For Linux
    # make sure the following packages are installed:
    # https://github.com/juce-framework/JUCE/blob/master/docs/Linux%20Dependencies.md
    # udo apt install libasound2-dev libjack-jackd2-dev \
    #    ladspa-sdk \
    #    libcurl4-openssl-dev  \
    #    libfreetype6-dev \
    #    libx11-dev libxcomposite-dev libxcursor-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev \
    #    libwebkit2gtk-4.0-dev \
    #    libglu1-mesa-dev mesa-common-dev
    # Print a message notifying about above packages
    message(STATUS "Make sure the following packages are installed: see https://github.com/juce-framework/JUCE/blob/master/docs/Linux%20Dependencies.md")
    message(STATUS "libasound2-dev libjack-jackd2-dev ladspa-sdk libcurl4-openssl-dev libfreetype6-dev libx11-dev libxcomposite-dev libxcursor-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libwebkit2gtk-4.0-dev libglu1-mesa-dev mesa-common-dev")

endif()

# Fetching JUCE
CPMAddPackage("gh:juce-framework/JUCE#develop")
# rn I'm using version 7.0.5 on commit # 69795dc
# CPMAddPackage("gh:juce-framework/JUCE#69795dc")
# Modules and subdirectories

juce_add_modules(shared_plugin_helpers)
if (${BUILD_JUCE_PROTOTYPE_EXAMPLES})
    #adding project folders:
endif()
add_subdirectory(PluginCode)
