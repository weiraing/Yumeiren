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

    // 上传后会释放 CPU 位图；上下文重建时必须重新解码。
    if (!m_setting || m_textureImages.size() != int(m_setting->GetTextureCount())) {
        if (!decodeTextures(outError)) {
            return false;
        }
    }

    // CreateRenderer 会访问着色器单例，必须先处理上下文换代。
    cubismruntime::syncShaderCache(contextGeneration);

    CreateRenderer(static_cast<csmUint32>(pixelSize.width()),
                   static_cast<csmUint32>(pixelSize.height()), 1);
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
        // Cubism 强制使用 mipmap 采样；缺失层级会导致纹理显示为黑色。
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
    // 上传完成后释放 CPU 位图，降低常驻内存。
    m_textureImages.clear();
    m_textureImages.squeeze();
    logInfo(QStringLiteral("纹理上传完成：%1 张，绘制面 %2x%3")
                .arg(static_cast<int>(m_textureIds.size()))
                .arg(pixelSize.width())
                .arg(pixelSize.height()));
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

// 按画布和视口比例修正投影，保持等比显示。
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
    // 清成 alpha=0：透明窗口下任何非零底色的都会变成立像周围一圈脏边。
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
