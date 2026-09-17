#include "kanban/Live2DRenderer.h"
#include "kanban/OffscreenGlHost.h"

#include <QApplication>
#include <QFileInfo>
#include <QImage>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QRegularExpression>
#include <QTextStream>

#include <cmath>
#include <stdexcept>

namespace {

void require(bool condition, const QString &message)
{
    if (!condition) {
        throw std::runtime_error(message.toStdString());
    }
}

// 每轮使用独立上下文，世代号模拟窗口关闭后重新启用。
class TestGlHost final : public kanban::OffscreenGlHost
{
public:
    TestGlHost(QOpenGLContext *context, QOffscreenSurface *surface,
               const QSize &size, quint64 generation)
        : OffscreenGlHost(context, surface, size), m_generation(generation) {}

    quint64 glContextGeneration() const override { return m_generation; }

private:
    quint64 m_generation;
};

bool hasModelPixels(const QImage &source)
{
    const QImage image = source.convertToFormat(QImage::Format_RGBA8888);
    bool nonblack = false;
    bool nonuniform = false;
    int firstColor = -1;
    int visible = 0;
    for (int y = 0; y < image.height(); ++y) {
        const uchar *row = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const uchar *pixel = row + x * 4;
            if (pixel[3] == 0) {
                continue;
            }
            ++visible;
            const int color = (pixel[0] << 16) | (pixel[1] << 8) | pixel[2];
            nonblack |= color != 0;
            if (firstColor < 0) {
                firstColor = color;
            } else {
                nonuniform |= color != firstColor;
            }
        }
    }
    return visible > 100 && visible < image.width() * image.height()
           && nonblack && nonuniform;
}

double targetX(const kanban::Live2DRenderer &renderer)
{
    static const QRegularExpression pattern(QStringLiteral("目标=\\(([-0-9.]+),"));
    const auto match = pattern.match(renderer.gazeDebugText());
    require(match.hasMatch(), QStringLiteral("缺少视线诊断目标"));
    return match.captured(1).toDouble();
}

void checkGaze(kanban::Live2DRenderer &renderer)
{
    renderer.setGazeStrength(kanban::KanbanRenderer::GazeWeak);
    renderer.pointerMove(QPointF(-100.0, 40.0));
    const double weak = std::abs(targetX(renderer));
    // 不重新发送 pointerMove，验证负坐标在切档时仍能立即重算。
    renderer.setGazeStrength(kanban::KanbanRenderer::GazeStrong);
    require(std::abs(targetX(renderer)) > weak + 0.02,
            QStringLiteral("负坐标切档未重算视线"));
    renderer.setGazeStrength(kanban::KanbanRenderer::GazeOff);
    require(std::abs(targetX(renderer)) < 0.001, QStringLiteral("关闭视线未归零"));
    renderer.setGazeStrength(kanban::KanbanRenderer::GazeMedium);
}

void checkContext(const QString &modelPath, quint64 generation)
{
    const QSize size(320, 480);
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::NoProfile);
    format.setAlphaBufferSize(8);
    format.setSamples(0);
    QOpenGLContext context;
    context.setFormat(format);
    require(context.create(), QStringLiteral("创建上下文失败"));
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    require(surface.isValid() && context.makeCurrent(&surface), QStringLiteral("上下文不可用"));
    QOpenGLFramebufferObject fbo(size, QOpenGLFramebufferObject::CombinedDepthStencil);
    require(fbo.isValid(), QStringLiteral("创建绘制目标失败"));
    TestGlHost host(&context, &surface, size, generation);
    // 析构顺序保证 renderer 先于宿主、FBO 和上下文释放。
    kanban::Live2DRenderer renderer;
    renderer.resize(size.width(), size.height(), 1.0f);
    renderer.setDeterministicIdle(true);
    QString error;
    require(!renderer.loadModel(modelPath + QStringLiteral(".missing"), &error)
                && !error.isEmpty(), QStringLiteral("缺失模型未返回错误"));

    for (int cycle = 0; cycle < 10; ++cycle) {
        // 首轮先装载再交宿主，覆盖首帧延迟上传。
        require(renderer.loadModel(modelPath, &error), error);
        renderer.setGlHost(&host);
        renderer.setGlHost(&host);
        checkGaze(renderer);
        require(fbo.bind(), QStringLiteral("绑定绘制目标失败"));
        for (int frame = 0; frame < 30; ++frame) {
            renderer.update(1.0f / 30.0f);
            renderer.render();
        }
        require(context.functions()->glGetError() == GL_NO_ERROR,
                QStringLiteral("渲染产生 GL 错误"));
        require(hasModelPixels(fbo.toImage()), QStringLiteral("模型空白、纯黑或缺少透明背景"));
        if (renderer.expressionCount() > 0) {
            require(renderer.playNextExpression(), QStringLiteral("表情未能播放"));
        }
        if (renderer.playableMotionCount() > 0) {
            require(renderer.playNextMotion(), QStringLiteral("动作未能播放"));
        }
        renderer.update(1.0f / 30.0f);
        // 主动归还上下文，验证卸载自行借用并归还上下文。
        context.doneCurrent();
        renderer.unloadModel();
        require(!QOpenGLContext::currentContext(), QStringLiteral("卸载后未归还上下文"));
        renderer.unloadModel();
        renderer.shutdown();
        renderer.shutdown();
        require(context.makeCurrent(&surface), QStringLiteral("重新取得上下文失败"));
        require(context.functions()->glGetError() == GL_NO_ERROR,
                QStringLiteral("释放产生 GL 错误"));
    }
    renderer.setGlHost(nullptr);
    QTextStream(stdout) << "context " << generation << ": 10 load/render/unload cycles PASS\n";
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    try {
        require(app.arguments().size() == 2, QStringLiteral("需要模型路径参数"));
        const QString modelPath = QFileInfo(app.arguments().at(1)).absoluteFilePath();
        require(QFileInfo::exists(modelPath), QStringLiteral("测试模型不存在"));
        for (quint64 generation = 1; generation <= 3; ++generation) {
            checkContext(modelPath, generation);
        }
        return 0;
    } catch (const std::exception &error) {
        QTextStream(stderr) << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
