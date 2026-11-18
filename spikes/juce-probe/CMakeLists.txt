cmake_minimum_required(VERSION 3.22)
project(dz_juce_spike VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/9.0.5.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(dz_spike
    PRODUCT_NAME "DhunZilla Spike"
    COMPANY_NAME "DhunZilla")

target_sources(dz_spike PRIVATE Source/Main.cpp)
target_compile_definitions(dz_spike PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="DhunZilla Spike"
    JUCE_APPLICATION_VERSION_STRING="0.1.0")
target_link_libraries(dz_spike PRIVATE
    juce::juce_audio_utils
    juce::juce_gui_extra
    PUBLIC juce::juce_recommended_config_flags
           juce::juce_recommended_warning_flags)
