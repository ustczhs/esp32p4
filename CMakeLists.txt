# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# esp-claw subsystem components are only discovered when CLAW_ENABLE=y.
# Kconfig has not run at this point, so read this build's sdkconfig directly
# (SDKCONFIG may point outside the project root); on a clean build it does not
# exist yet, so also scan SDKCONFIG_DEFAULTS. The decision is published as a
# build property because main's dependency-collection pass runs in a separate
# CMake process without CONFIG_ variables or this directory scope.
set(CLAW_SDKCONFIG "${SDKCONFIG}")
if(NOT CLAW_SDKCONFIG)
    set(CLAW_SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
elseif(NOT IS_ABSOLUTE "${CLAW_SDKCONFIG}")
    set(CLAW_SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/${CLAW_SDKCONFIG}")
endif()
set(CLAW_BUILD_ENABLED OFF)
if(EXISTS "${CLAW_SDKCONFIG}")
    file(STRINGS "${CLAW_SDKCONFIG}" CLAW_ENABLE_LINE REGEX "^CONFIG_CLAW_ENABLE=y$")
    if(CLAW_ENABLE_LINE)
        set(CLAW_BUILD_ENABLED ON)
    endif()
endif()
if(NOT CLAW_BUILD_ENABLED AND NOT EXISTS "${CLAW_SDKCONFIG}" AND DEFINED SDKCONFIG_DEFAULTS)
    foreach(claw_defaults_file ${SDKCONFIG_DEFAULTS})
        if(NOT IS_ABSOLUTE "${claw_defaults_file}")
            set(claw_defaults_file "${CMAKE_CURRENT_LIST_DIR}/${claw_defaults_file}")
        endif()
        if(EXISTS "${claw_defaults_file}")
            file(STRINGS "${claw_defaults_file}" CLAW_DEFAULT_LINE REGEX "^CONFIG_CLAW_ENABLE=y$")
            if(CLAW_DEFAULT_LINE)
                set(CLAW_BUILD_ENABLED ON)
            endif()
        endif()
    endforeach()
endif()
if(CLAW_BUILD_ENABLED)
    list(APPEND EXTRA_COMPONENT_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/components/esp_claw/claw_modules"
        "${CMAKE_CURRENT_LIST_DIR}/components/esp_claw/claw_capabilities"
        "${CMAKE_CURRENT_LIST_DIR}/components/esp_claw/common")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(CLAW_BUILD_ENABLED ${CLAW_BUILD_ENABLED})
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.5.0")
project(xiaozhi)
