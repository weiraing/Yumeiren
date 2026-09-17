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
target_link_libraries(KanbanControllerTest PRIVATE Qt6::Widgets dwmapi psapi)
yumeiren_deploy_qt_runtime(KanbanControllerTest)
add_test(NAME KanbanControllerSettings COMMAND KanbanControllerTest)
set_tests_properties(KanbanControllerSettings PROPERTIES TIMEOUT 30)
