# 界面辅助函数的回归（不弹窗口、不读用户配置）。
option(YUMEIREN_BUILD_UI_TESTS "构建界面辅助函数回归" OFF)
if(NOT YUMEIREN_BUILD_UI_TESTS)
    return()
endif()

enable_testing()
add_executable(TooltipStyleTest
    tests/ui/TooltipStyleTest.cpp
    src/ui/TooltipStyle.cpp
)
set_target_properties(TooltipStyleTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>/ui-test")
target_include_directories(TooltipStyleTest PRIVATE src)
yumeiren_apply_version(TooltipStyleTest)
# format() 里要读 QApplication::font()，所以只能是 QApplication 而不是 QCoreApplication；
# 于是这里必须链接 Widgets 并部署平台插件，否则 ctest 里会在构造阶段直接 abort。
# （不会真的弹窗：测试全程不 show() 任何控件。）
target_link_libraries(TooltipStyleTest PRIVATE Qt6::Widgets)
yumeiren_deploy_qt_runtime(TooltipStyleTest)
add_test(NAME TooltipStyle COMMAND TooltipStyleTest)
set_tests_properties(TooltipStyle PROPERTIES TIMEOUT 30
    # Qt 在 Windows 上只在 stderr 是**真控制台**时才写日志（内部拿
    # GetConsoleMode 探），ctest 是用管道收输出的，于是 qCritical/qInfo 全被改道去
    # OutputDebugString —— `ctest --output-on-failure` 会一个字都看不到
    # （2026-09-18 实测：不加这个变量时连「N checks passed」都不出现）。
    # QT_FORCE_STDERR_LOGGING 是 Qt 为此留的开关，只影响本测试进程。
    ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
