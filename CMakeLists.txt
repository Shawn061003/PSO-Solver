cmake_minimum_required(VERSION 3.20)

project(modeling_pso_solver VERSION 0.1.0 LANGUAGES CXX)

option(PSO_BUILD_TESTS "Build the test executables" ON)

add_library(pso_solver
    src/config.cpp
    src/pso.cpp
)
add_library(pso::solver ALIAS pso_solver)

target_include_directories(pso_solver
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(pso_solver PUBLIC cxx_std_20)

if(MSVC)
    # 公共头文件含中文注释；/utf-8 必须传递给所有使用者，避免本地代码页误解析。
    target_compile_options(pso_solver PUBLIC /utf-8 PRIVATE /W4 /permissive-)
else()
    target_compile_options(pso_solver PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PSO_BUILD_TESTS)
    include(CTest)
    add_executable(test_pso tests/test_pso.cpp)
    target_link_libraries(test_pso PRIVATE pso::solver)
    add_test(NAME pso_core COMMAND test_pso)

    add_executable(test_config tests/test_config.cpp)
    target_link_libraries(test_config PRIVATE pso::solver)

    # 将测试配置复制到构建目录。这样测试程序只需访问构建目录中的路径，
    # 即使源码仓库位于含中文或空格的目录下，也不会受 Windows 本地代码页影响。
    set(PSO_TEST_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/test_data")
    file(MAKE_DIRECTORY "${PSO_TEST_CONFIG_DIR}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/config/pso.yaml"
        "${PSO_TEST_CONFIG_DIR}/pso.yaml"
        COPYONLY
    )
    target_compile_definitions(test_config PRIVATE
        TEST_CONFIG_PATH="test_data/pso.yaml"
    )
    add_test(NAME yaml_config COMMAND test_config)
    set_tests_properties(yaml_config PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
endif()

install(TARGETS pso_solver
    EXPORT modeling_pso_solver_targets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(DIRECTORY include/ DESTINATION include)
