add_library(mavsdk)

cmake_policy(SET CMP0076 NEW)

add_subdirectory(core)
add_subdirectory(plugins)

hunter_add_package(jsoncpp)
include("${DEPS_INSTALL_PATH}/lib/cmake/jsoncpp/jsoncppConfig.cmake" OPTIONAL)
if(NOT TARGET JsonCpp::JsonCpp)
    find_package(jsoncpp REQUIRED)
endif()

hunter_add_package(tinyxml2)
include("${DEPS_INSTALL_PATH}/lib/cmake/tinyxml2/tinyxml2-config.cmake" OPTIONAL)
if(NOT TARGET tinyxml2::tinyxml2)
    find_package(tinyxml2 REQUIRED)
endif()

target_link_libraries(mavsdk
    PRIVATE
    JsonCpp::JsonCpp
    tinyxml2::tinyxml2
)

target_compile_definitions(mavsdk PUBLIC
    MAVLINK_EXTERNAL_RX_STATUS
    MAVLINK_EXTERNAL_RX_BUFFER
    )

if (NOT APPLE AND NOT ANDROID AND NOT MSVC)
    target_link_libraries(mavsdk
        PRIVATE
        stdc++fs
    )
endif()

set(UNIT_TEST_SOURCES ${UNIT_TEST_SOURCES} PARENT_SCOPE)
