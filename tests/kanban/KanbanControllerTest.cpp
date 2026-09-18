// 直接验证控制器配置契约，不启动桌面窗口或读取用户配置。
#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "kanban/KanbanController.h"
#include "kanban/KanbanAnimationClock.h"
#include "kanban/KanbanRenderer.h"

#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryDir>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    // 配置路径固定相对程序目录，测试必须在专用输出目录运行。
    if (QDir(QCoreApplication::applicationDirPath()).dirName()
        != QStringLiteral("controller-test")) {
        qCritical() << "Test executable must stay in controller-test directory";
        return 1;
    }
    AppConfig &config = AppConfig::instance();
    for (const QString &key : config.allKeys()) {
        config.remove(key);
    }
    int failures = 0;
    int checks = 0;
    const auto check = [&](bool condition, const char *message) {
        ++checks;
        if (!condition) {
            ++failures;
            qCritical() << message;
        }
    };
    // 从其他工作目录启动时，模型仍从可执行文件旁读取。
    const QString originalWorkingDirectory = QDir::currentPath();
    const QString expectedModelsRoot = QDir(QCoreApplication::applicationDirPath())
                                          .absoluteFilePath(QStringLiteral("data/models"));
    QTemporaryDir workingDirectory;
    check(workingDirectory.isValid() && QDir::setCurrent(workingDirectory.path()),
          "Temporary working directory must be available");
    check(kanban::KanbanModelManager::defaultModelsRoot() == expectedModelsRoot,
          "Model root must be fixed relative to executable, not working directory");
    check(QDir::setCurrent(originalWorkingDirectory), "Working directory must be restored");

    // 删掉当前模型后该切到哪一个。三处边界都容易写错，逐个钉住。
    // 语义：preCount 个模型里删掉下标 i，剩下 postCount = preCount - 1 个。
    {
        // 中间：删掉第 1 个(共 5 个 → 剩 4 个)，原来的第 2 个补到第 1 位，
        // 所以「下一个」在下标 1。**不是**第一个(0)。
        check(kanban::KanbanModelManager::successorIndexAfterRemoval(1, 4) == 1,
              "Removing a middle model must select the one that shifted into its slot");
        // 第一个：删掉第 0 个(剩 4 个)，原来的第 1 个补到第 0 位 → 下标 0。
        // 这条与「切到第一个」结果相同，但原因不同，别当成冗余删掉。
        check(kanban::KanbanModelManager::successorIndexAfterRemoval(0, 4) == 0,
              "Removing the first model must select the one that shifted into slot 0");
        // 最后一个：没有下一个 → 绕回第一个。removedIndex(=4) 已经越界。
        check(kanban::KanbanModelManager::successorIndexAfterRemoval(4, 4) == 0,
              "Removing the last model must wrap around to the first");
        // 找不到(下标无效)：兜底到第一个，别越界。
        check(kanban::KanbanModelManager::successorIndexAfterRemoval(-1, 4) == 0,
              "Unknown removed index must fall back to the first model");
        // 删完一个不剩：调用方会判空，这里只保证不越界。
        check(kanban::KanbanModelManager::successorIndexAfterRemoval(0, 0) == 0,
              "No remaining models must not produce an out-of-range index");
    }

    const QString strengthKey = QString::fromLatin1(ConfigKeys::Kanban::GazeStrength);
    const QString legacyKey = QString::fromLatin1(ConfigKeys::Kanban::GazeTrackingLegacy);
    const QString enabledKey = QString::fromLatin1(ConfigKeys::Kanban::Enabled);
    config.setValue(legacyKey, false);
    {
        kanban::KanbanController controller;
        check(!controller.wasRunningLastTime(), "First launch must not auto-start");
        check(!controller.gazeTracking(), "Legacy false must disable gaze");
        check(config.value(strengthKey).toInt() == 0, "Migration must persist new key");

        config.setValue(legacyKey, true);
        controller.loadSettings();
        check(!controller.gazeTracking(), "New strength key must override legacy value");
        config.remove(strengthKey);
        controller.loadSettings();
        check(controller.gazeStrength() == kanban::KanbanRenderer::GazeMedium,
              "Legacy true must migrate to medium");

        controller.setGazeStrength(99);
        check(controller.gazeStrength() == 3 && controller.gazeTracking(), "Gaze upper limit");
        check(config.value(strengthKey).toInt() == 3, "Gaze must persist");
        controller.setGazeStrength(-1);
        check(controller.gazeStrength() == 0 && !controller.gazeTracking(), "Gaze off limit");
        check(!config.value(legacyKey).toBool(), "Gaze off must synchronize legacy key");

        controller.setScalePercent(-1);
        controller.setOpacityPercent(-1);
        controller.setTargetFps(-1);
        check(controller.scalePercent() == 20, "Scale lower limit");
        check(controller.opacityPercent() == 20, "Opacity lower limit");
        check(controller.targetFps() == 10, "FPS lower limit");
        controller.setScalePercent(999);
        controller.setOpacityPercent(999);
        controller.setTargetFps(999);
        check(controller.scalePercent() == 300, "Scale upper limit");
        check(controller.opacityPercent() == 100, "Opacity upper limit");
        check(controller.targetFps() == 60, "FPS upper limit");
        controller.setAlwaysOnTop(false);
        controller.setMouseThrough(true);
        controller.setInteractionEnabled(false);
        controller.loadSettings();
        check(controller.scalePercent() == 300 && controller.opacityPercent() == 100
                  && controller.targetFps() == 60, "Numeric settings must survive reload");
        check(!controller.alwaysOnTop() && controller.mouseThrough()
                  && !controller.interactionEnabled(), "Boolean settings must survive reload");

        check(!controller.setModelPath(QStringLiteral("missing.model3.json")),
              "Unknown model must be rejected");
        check(!controller.lastError().isEmpty(), "Rejected model must report error");
        controller.playNext();
        controller.playNextExpression();
        check(!controller.isRunning(), "Stopped interaction must not start controller");

        config.setValue(enabledKey, true);
        controller.shutdownForExit();
        controller.shutdownForExit();
        check(controller.wasRunningLastTime(), "Exit must preserve enabled preference");
        controller.stop();
        check(!controller.wasRunningLastTime(), "Explicit stop must clear enabled preference");

        // 不建窗口，验证真实时钟统计经控制器转发；通知频率不能退化为逐帧。
        auto *clock = controller.findChild<kanban::KanbanAnimationClock *>();
        check(clock != nullptr, "Controller must own an animation clock");
        if (clock) {
            int ticks = 0;
            int updates = 0;
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            QObject::connect(clock, &kanban::KanbanAnimationClock::tick,
                             &loop, [&](float delta) {
                ++ticks;
                check(delta >= 0.0f && delta <= 0.1f, "Frame delta must stay bounded");
            });
            QObject::connect(&controller, &kanban::KanbanController::measuredFpsChanged,
                             &loop, [&] {
                ++updates;
                loop.quit();
            });
            clock->setTargetFps(30);
            clock->start();
            clock->start();
            timeout.start(5000);
            loop.exec();
            check(updates == 1 && ticks > updates, "FPS must be forwarded at sampling rate");
            check(controller.measuredFps() > 0, "Running clock must publish nonzero FPS");
            clock->stop();
            check(controller.measuredFps() == 0 && updates == 2,
                  "Stop must publish zero FPS");
            clock->stop();
            check(updates == 2, "Repeated stop must not publish duplicate updates");
            clock->start();
            timeout.start(5000);
            loop.exec();
            check(updates == 3 && controller.measuredFps() > 0,
                  "Restart must resume FPS notifications");
            clock->stop();
        }
    }
    check(config.save(), "Test configuration must save successfully");
    qInfo() << checks << "checks," << failures << "failures";
    return failures == 0 ? 0 : 1;
}
