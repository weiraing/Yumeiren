# 不依赖 Cubism SDK；测试配置与产品配置按程序输出目录隔离。
option(YUMEIREN_BUILD_CONTROLLER_TESTS "构建看板娘控制器配置回归" OFF)
if(NOT YUMEIREN_BUILD_CONTROLLER_TESTS)
    return()
endif()

enable_testing()
add_executable(KanbanControllerTest
    tests/kanban/KanbanControllerTest.cpp
    src/kanban/KanbanController.cpp
    src/kanban/KanbanControllerSettings.cpp
    src/kanban/KanbanControllerInteraction.cpp
    src/kanban/KanbanAnimationClock.cpp
    src/kanban/KanbanModelManager.cpp
    src/kanban/KanbanStateMachine.cpp
    src/kanban/KanbanWindow.cpp
    src/kanban/KanbanSoftwareView.cpp
    src/kanban/PlaceholderRenderer.cpp
    src/kanban/Live2DRendererStub.cpp
    src/app/ApplicationRuntimeState.cpp
    src/config/AppConfig.cpp
    src/core/Diagnostics.cpp
    src/core/CachePaths.cpp
)
set_target_properties(KanbanControllerTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>/controller-test")
target_include_directories(KanbanControllerTest PRIVATE src)
# 同上：现在用不到版本号，接上生成头文件是为了将来加 AppInfo.cpp 时不踩坑。
yumeiren_apply_version(KanbanControllerTest)
target_link_libraries(KanbanControllerTest PRIVATE Qt6::Widgets dwmapi psapi)
yumeiren_deploy_qt_runtime(KanbanControllerTest)
add_test(NAME KanbanControllerSettings COMMAND KanbanControllerTest)
set_tests_properties(KanbanControllerSettings PROPERTIES TIMEOUT 30
    # 不加这个变量时 qCritical 会被 Qt 改道去 OutputDebugString（它只在 stderr 是
    # 真控制台时才写 stderr，而 ctest 用管道收输出），`--output-on-failure` 看不到
    # 任何失败原因。理由详见 cmake/UiTests.cmake。
    ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
