#include <QApplication>
#include <QMediaPlayer>
#include <QPainter>
#include <QPaintEvent>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWidget>

// QVideoSink A/B 实验探针(第二阶段内存归因专用，非产品组件，可整体删除)：
//   QMediaPlayer → QVideoSink → 仅保留最新帧 → 每帧 toImage() 后 QPainter 绘制
// 与生产路径 QMediaPlayer → QVideoWidget(零拷贝 GPU 呈现) 对照，回答：
// Private Bytes 是否下降 / 显存是否下降 / CPU 是否上升 / 是否引入额外帧复制。
// 用法：SinkProbe.exe <媒体文件|none> [运行秒数(默认90)]；"none" 为空闲基线。

namespace {

class FrameView : public QWidget
{
public:
    explicit FrameView(QWidget *parent = nullptr) : QWidget(parent)
    {
        // 与 QVideoWidget 壁纸窗口同样的无边界/点击穿透语义
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowTransparentForInput);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    void setLatestFrame(const QVideoFrame &frame)
    {
        m_frame = frame; // 仅保留最新帧：旧帧随赋值立即释放
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (!m_frame.isValid())
            return;
        const QImage img = m_frame.toImage(); // 每帧一次 CPU 侧映射/下载+格式转换
        if (img.isNull())
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(rect(), img);
    }

private:
    QVideoFrame m_frame;
};

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2)
        return 1;
    const bool idleBaseline = (args.at(1) == QLatin1String("none"));
    const int durationSec = args.size() >= 3 ? qMax(5, args.at(2).toInt()) : 90;

    FrameView view;
    view.resize(960, 540);
    view.show();

    QMediaPlayer player;
    QVideoSink sink;
    if (!idleBaseline) {
        player.setVideoSink(&sink);
        QObject::connect(&sink, &QVideoSink::videoFrameChanged, &view,
                         [&view](const QVideoFrame &frame) {
                             view.setLatestFrame(frame);
                         });
        player.setSource(QUrl::fromLocalFile(args.at(1)));
        player.setLoops(QMediaPlayer::Infinite);
        player.play();
    }

    QTimer::singleShot(durationSec * 1000, &app, &QCoreApplication::quit);
    return app.exec();
}
