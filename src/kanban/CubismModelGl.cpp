#include "kanban/CubismModel_p.h"
#include "kanban/CubismRuntime.h"

#include "Math/CubismModelMatrix.hpp"
#include "Rendering/OpenGL/CubismOffscreenManager_OpenGLES2.hpp"
#include "Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"

namespace kanban::detail {

using cubismruntime::logInfo;
using namespace Live2D::Cubism::Framework::Rendering;

bool CubismModelImpl::ensureGl(const QSize &pixelSize, quint64 contextGeneration,
                                 QString *outError)
{
    if (!_model) {
        if (outError) {
            *outError = QStringLiteral("模型未装载，无法建立 GL 资源");
        }
        return false;
    }
    releaseGl(); // 重建前先彻底清干净，纹理与 VBO 才不会有累积

    // 解码失败过一次就不再重试：本函数由动画时钟逐帧调用，重试会成每帧读盘解码。
    if (!m_decodeError.isEmpty()) {
        if (outError) {
            *outError = m_decodeError;
        }
        return false;
    }

    // 上传后会释放 CPU 位图；上下文重建时必须按当次上限重新解码。
    const int maxDim = textureMaxDimFor(pixelSize);
    if (!m_setting || m_textureMaxDim != maxDim
        || m_textureImages.size() != int(m_setting->GetTextureCount())) {
        if (!decodeTextures(maxDim, outError)) {
            return false;
        }
        m_textureMaxDim = maxDim;
    }

    // CreateRenderer 会访问着色器单例，必须先处理上下文换代。
    cubismruntime::syncShaderCache(contextGeneration);

    // 遮罩上限：SDK 用 1 张 render texture 时最多排 36 个遮罩组(9 宫格 × RGBA)，
    // 2 张起每张按 32 个递增(见 CubismClippingManager 的 ClippingMaskMaxCount*)。
    // ⚠️ 超限时 SDK **不会中止** —— SetupLayoutBounds 只塞一组凑数的布局就返回，
    // 源码注释自己写明「继续执行的话 SetupShaderProgram 会越界访问」，而那句
    // CSM_ASSERT(0) 在 Release 下是空的(#define CSM_ASSERT(expr))。
    // 后果是渲染一帧后堆被写坏，直到下一次内存分配才崩 —— 实测崩在**下一帧的
    // update()** 里，表现为「模型能装载、能画首帧，然后毫无征兆地段错误」。
    // 所以按遮罩组数**上界**(有遮罩的 drawable 数；SDK 还会对相同遮罩集去重，
    // 实际只会更少)预先申请足够张数。
    csmInt32 maskBufferCount = 1;
    csmInt32 maskedDrawableCount = 0;
    {
        const csmInt32 drawableCount = _model->GetDrawableCount();
        const csmInt32 *maskCounts = _model->GetDrawableMaskCounts();
        for (csmInt32 i = 0; i < drawableCount; ++i) {
            if (maskCounts && maskCounts[i] > 0) {
                ++maskedDrawableCount;
            }
        }
        if (maskedDrawableCount > 36) {
            maskBufferCount = (maskedDrawableCount + 31) / 32;
            logInfo(QStringLiteral("遮罩组数 %1 超过单张上限 36，render texture 申请 %2 张")
                        .arg(maskedDrawableCount)
                        .arg(maskBufferCount));
        }
    }

    CreateRenderer(static_cast<csmUint32>(pixelSize.width()),
                   static_cast<csmUint32>(pixelSize.height()), maskBufferCount);
    CubismRenderer_OpenGLES2 *renderer = GetRenderer<CubismRenderer_OpenGLES2>();
    if (!renderer) {
        if (outError) {
            *outError = QStringLiteral("Cubism 渲染器创建失败(GL 上下文未就绪?)");
        }
        return false;
    }

    for (int i = 0; i < m_textureImages.size(); ++i) {
        const QImage &image = m_textureImages.at(i);
        if (image.isNull()) {
            continue; // 空名字纹理位：model3.json 允许留空槽
        }
        GLuint textureId = 0;
        glGenTextures(1, &textureId);
        glBindTexture(GL_TEXTURE_2D, textureId);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width(), image.height(), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
        // Cubism 强制 mipmap 采样；缺失层级会导致纹理显示为黑色。
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // 限制边缘采样，避免重复纹理产生杂色。
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        renderer->BindTexture(static_cast<csmUint32>(i), textureId);
        m_textureIds.push_back(textureId);
    }
    // 上传前已在 CPU 侧乘过 alpha，这里必须如实告知，否则二次相乘会让边缘发暗。
    renderer->IsPremultipliedAlpha(true);
    m_glLive = true;
    // 日志要报实际上传尺寸，必须在释放 CPU 位图之前取值。
    const QSize uploadedSize =
        m_textureImages.isEmpty() ? QSize() : m_textureImages.first().size();
    m_textureImages.clear();
    m_textureImages.squeeze();
    logInfo(QStringLiteral("纹理上传完成：%1 张，绘制面 %2x%3，上限 %4，实际 %5x%6")
                .arg(static_cast<int>(m_textureIds.size()))
                .arg(pixelSize.width())
                .arg(pixelSize.height())
                .arg(maxDim > 0 ? QString::number(maxDim) : QStringLiteral("原尺寸"))
                .arg(uploadedSize.width())
                .arg(uploadedSize.height()));
    return true;
}

void CubismModelImpl::releaseGl()
{
    if (!m_glLive) {
        m_textureIds.clear();
        return;
    }
    m_glLive = false;
    if (!m_textureIds.empty()) {
        glDeleteTextures(static_cast<GLsizei>(m_textureIds.size()), m_textureIds.data());
        m_textureIds.clear();
    }
    DeleteRenderer(); // 基类实现自带空指针判定，重复调用安全
}

void CubismModelImpl::fitProjection(const QSize &pixelSize, CubismMatrix44 *out)
{
    const float w = pixelSize.width() > 0 ? static_cast<float>(pixelSize.width()) : 1.0f;
    const float h = pixelSize.height() > 0 ? static_cast<float>(pixelSize.height()) : 1.0f;

    if (_model->GetCanvasWidth() > 1.0f && w < h) {
        _modelMatrix->SetWidth(2.0f); // 横向画布塞进竖窗口：以宽度为准
        m_projScaleX = 1.0f;
        m_projScaleY = w / h;
    } else {
        m_projScaleX = h / w;
        m_projScaleY = 1.0f;
    }
    out->LoadIdentity();
    out->Scale(m_projScaleX, m_projScaleY);
    out->MultiplyByMatrix(_modelMatrix);
}

void CubismModelImpl::draw(const QSize &pixelSize)
{
    CubismRenderer_OpenGLES2 *renderer = GetRenderer<CubismRenderer_OpenGLES2>();
    if (!_model || !renderer) {
        return;
    }
    glViewport(0, 0, pixelSize.width(), pixelSize.height());
    // 必须清成 alpha=0：透明窗口下任何非零底色都会在立像周围留一圈脏边。
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    ICubismOffscreenManager<CubismRenderTarget_OpenGLES2> *offscreen =
        CubismOffscreenManager_OpenGLES2::GetInstance();
    offscreen->BeginFrameProcess();

    CubismMatrix44 projection;
    fitProjection(pixelSize, &projection);
    renderer->SetMvpMatrix(&projection);
    renderer->DrawModel();

    offscreen->EndFrameProcess();
    offscreen->ReleaseStaleRenderTextures();
}

} // namespace kanban::detail
