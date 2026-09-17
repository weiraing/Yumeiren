# 可选的真实 SDK 离屏回归，不链接主窗口或修改用户设置。
option(YUMEIREN_BUILD_RENDER_TESTS "构建 Cubism 生命周期回归测试" OFF)
if(NOT YUMEIREN_BUILD_RENDER_TESTS)
    return()
endif()
if(NOT YUMEIREN_WITH_LIVE2D)
    message(FATAL_ERROR "渲染回归需要 YUMEIREN_WITH_LIVE2D=ON")
endif()

enable_testing()
add_executable(CubismLifecycleTest
    tests/kanban/CubismLifecycleTest.cpp
    src/kanban/Live2DRendererCubism.cpp
    src/kanban/CubismRuntime.cpp
    src/kanban/CubismModel.cpp
    src/kanban/CubismModelGl.cpp
    src/kanban/CubismModelInteraction.cpp
    src/core/Diagnostics.cpp
    src/core/CachePaths.cpp
    src/config/AppConfig.cpp
)
target_include_directories(CubismLifecycleTest PRIVATE src)
target_link_libraries(CubismLifecycleTest PRIVATE ${YUMEIREN_LIBS})
yumeiren_deploy_cubism(CubismLifecycleTest)
yumeiren_deploy_qt_runtime(CubismLifecycleTest)

set(YUMEIREN_TEST_MODEL "${CMAKE_SOURCE_DIR}/data/models/Haru/Haru.model3.json"
    CACHE FILEPATH "生命周期回归使用的模型")
add_test(NAME CubismLifecycle
    COMMAND CubismLifecycleTest "${YUMEIREN_TEST_MODEL}")
set_tests_properties(CubismLifecycle PROPERTIES TIMEOUT 120)
