cmake_minimum_required(VERSION 3.16)
project(lifelong LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()

# Set the default value of PYTHON to false
option(PYTHON "Enable Python" OFF)

if(PYTHON)
    set(PYTHON_FLAG 1)
else()
    set(PYTHON_FLAG 0)
endif()


# Print the value of PYTHON
message(STATUS "PYTHON: ${PYTHON}")

IF(NOT CMAKE_BUILD_TYPE)
    # SET(CMAKE_BUILD_TYPE "DEBUG")
    SET(CMAKE_BUILD_TYPE "RELEASE")
    #SET(CMAKE_BUILD_TYPE "RELWITHDEBINFO")
    #SET(CMAKE_BUILD_TYPE "MINSIZEREL")
ENDIF()

include_directories("inc")

# default planner
include_directories("default_planner")

file(GLOB SOURCES "src/*.cpp" "default_planner/*.cpp")
list(FILTER SOURCES EXCLUDE REGEX ".*/src/tcp_server_main\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX ".*/src/PibtTcpServer\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX ".*/src/UnityStartKitAdapter\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX ".*/src/PlannerSession\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX ".*/src/epibt_smoke_main\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX ".*/src/dual_width_smoke_main\\.cpp$")


# Find Boost
ADD_DEFINITIONS(-DBOOST_LOG_DYN_LINK)
#SET(Boost_USE_STATIC_LIBS ON)
set(Boost_NO_BOOST_CMAKE ON)
find_package( Boost 1.49.0 REQUIRED COMPONENTS program_options filesystem log log_setup)

include_directories( ${Boost_INCLUDE_DIRS} )


IF(PYTHON)
    message(STATUS "Python support is enabled")

    find_package(pybind11 REQUIRED)
    set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ version selection")

     # check if python/tmp exists, raise an error if it does not
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/python/tmp")
        message(FATAL_ERROR "Track is not configured for python bindings. Please run python/set_track.bash <combined|planner|scheduler> to configure the track before compiling.")
    endif()

    include_directories("python/tmp")

    pybind11_add_module(MAPF "./src/Grid.cpp"   "./src/States.cpp" "./python/tmp/MAPFbinding.cpp")
    target_link_libraries(MAPF PRIVATE ) 

    file(GLOB PY_SOURCES "python/tmp/*.cpp")
    list(FILTER PY_SOURCES EXCLUDE REGEX ".*python/MAPFbinding\.cpp")

    add_executable(lifelong  ${PY_SOURCES} ${SOURCES})
    target_link_libraries(lifelong PRIVATE pybind11::embed ${Boost_LIBRARIES})
    target_compile_definitions(lifelong PRIVATE BOOST_ERROR_CODE_HEADER_ONLY PYTHON=${PYTHON_FLAG})

    if (COPY_PY_PATH_CONFIG)
        file(COPY "config.json" DESTINATION ${CMAKE_BINARY_DIR})
    endif()


ELSE()
    message(STATUS "Python support is disabled")
    

    add_executable(lifelong ${SOURCES} )
    target_link_libraries(lifelong ${Boost_LIBRARIES})
    target_compile_definitions(lifelong PRIVATE BOOST_ERROR_CODE_HEADER_ONLY PYTHON=${PYTHON_FLAG})
ENDIF()

# ── pibt_tcp_server (Sprint 05: links DefaultPlanner + adapter) ──────────────
# Glob all default_planner sources (shared with lifelong target)
file(GLOB TCP_PLANNER_SOURCES "default_planner/*.cpp")

add_executable(
    pibt_tcp_server
    src/tcp_server_main.cpp
    src/FileLogger.cpp
    src/PibtTcpServer.cpp
    src/PlannerSession.cpp
    src/UnityStartKitAdapter.cpp
    src/ActionModel.cpp
    src/Grid.cpp
    src/States.cpp
    src/common.cpp
    ${TCP_PLANNER_SOURCES}
)
target_include_directories(pibt_tcp_server PRIVATE inc default_planner ${Boost_INCLUDE_DIRS})
target_link_libraries(pibt_tcp_server PRIVATE ${Boost_LIBRARIES})
target_compile_definitions(pibt_tcp_server PRIVATE BOOST_ERROR_CODE_HEADER_ONLY PYTHON=${PYTHON_FLAG})

add_executable(
    epibt_smoke_test
    src/epibt_smoke_main.cpp
    src/States.cpp
    src/common.cpp
    default_planner/utils.cpp
    default_planner/heuristics.cpp
    default_planner/epibt.cpp
)
target_include_directories(epibt_smoke_test PRIVATE inc default_planner ${Boost_INCLUDE_DIRS})
target_link_libraries(epibt_smoke_test PRIVATE ${Boost_LIBRARIES})
target_compile_definitions(epibt_smoke_test PRIVATE BOOST_ERROR_CODE_HEADER_ONLY PYTHON=${PYTHON_FLAG})

# G-1: dual-width smoke test — catches `static delta` regression in NextLoc
add_executable(
    dual_width_smoke_test
    src/dual_width_smoke_main.cpp
    src/UnityStartKitAdapter.cpp
    src/ActionModel.cpp
    src/States.cpp
    src/common.cpp
)
target_include_directories(dual_width_smoke_test PRIVATE inc default_planner ${Boost_INCLUDE_DIRS})
target_link_libraries(dual_width_smoke_test PRIVATE ${Boost_LIBRARIES})
target_compile_definitions(dual_width_smoke_test PRIVATE BOOST_ERROR_CODE_HEADER_ONLY PYTHON=${PYTHON_FLAG})


add_custom_target(clean_all
    COMMAND ${CMAKE_BUILD_TOOL} clean
    COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_BINARY_DIR}/CMakeCache.txt
    COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_BINARY_DIR}/lifelong
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/build
    COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_BINARY_DIR}/*.so
)


set_directory_properties(PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES
    "${CMAKE_BINARY_DIR}/lifelong;
     ${CMAKE_BINARY_DIR}/build;
     ${CMAKE_BINARY_DIR}/*.so"
)
