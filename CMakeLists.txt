cmake_minimum_required(VERSION 3.19)
project(Delivery_Maro LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Configure with MSVC for Win32 (-A Win32).")
endif()

set(COCOS2DX_ROOT_PATH "$ENV{COCOS2DX_ROOT_PATH}" CACHE PATH "Cocos2d-x 4.0 SDK directory")
if(NOT EXISTS "${COCOS2DX_ROOT_PATH}/cocos/CMakeLists.txt" OR
   NOT EXISTS "${COCOS2DX_ROOT_PATH}/external/CMakeLists.txt")
    message(FATAL_ERROR "Set COCOS2DX_ROOT_PATH to the Cocos2d-x 4.0 SDK with its external dependencies.")
endif()

list(PREPEND CMAKE_MODULE_PATH "${COCOS2DX_ROOT_PATH}/cmake/Modules")
set(BUILD_EDITOR_COCOSTUDIO OFF CACHE BOOL "Build the Cocos Studio loader")
set(BUILD_EXTENSIONS OFF CACHE BOOL "Build the optional Cocos extensions")
include(CocosBuildSet)
add_subdirectory("${COCOS2DX_ROOT_PATH}/cocos" "${ENGINE_BINARY_PATH}/cocos/core")

add_executable(Delivery_Maro WIN32 Game_Maro.cpp Network_Maro.cpp Network_Maro.h Campaign_Maro.h)
target_link_libraries(Delivery_Maro cocos2d ws2_32)
target_compile_options(Delivery_Maro PRIVATE /utf-8)
set_target_properties(Delivery_Maro PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES)
setup_cocos_app_config(Delivery_Maro)
cocos_copy_target_dll(Delivery_Maro)

add_custom_target(DeliveryAssets_Maro
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets_Maro"
        "$<TARGET_FILE_DIR:Delivery_Maro>/Resources/Assets_Maro"
    VERBATIM)
add_dependencies(Delivery_Maro DeliveryAssets_Maro)
