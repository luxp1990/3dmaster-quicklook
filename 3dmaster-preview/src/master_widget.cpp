#include "master_widget.h"
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <cmath>
#include <unordered_set>

MasterWidget::MasterWidget(QWidget* parent)
    : QOpenGLWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);

    // 启用 4x MSAA 多重采样抗锯齿 (以实例级 setFormat 配置，严格遵守 Guest DLL 契约)
    QSurfaceFormat fmt = format();
    fmt.setSamples(4);
    setFormat(fmt);

    m_animTimer = new QTimer(this);
    connect(m_animTimer, &QTimer::timeout, this, [this]() {
        if (m_isLoading) {
            m_spinnerAngle += 6.0f;
            if (m_spinnerAngle >= 360.0f) m_spinnerAngle -= 360.0f;
            update();
        }
    });

    // 相机运镜平滑插值计时器 (60 FPS，三次平滑缓出 Cubic Ease-Out)
    m_cameraAnimTimer = new QTimer(this);
    connect(m_cameraAnimTimer, &QTimer::timeout, this, [this]() {
        if (!m_isCameraAnimating) return;
        qint64 elapsed = m_cameraAnimElapsed.elapsed();
        float t = static_cast<float>(elapsed) / static_cast<float>(m_cameraAnimDuration);
        if (t >= 1.0f) {
            m_rotation = m_animTargetRot;
            m_cameraTarget = m_animTargetTarget;
            m_cameraDistance = m_animTargetDist;
            stopCameraAnimation();
            updateProjectionMatrix();
            update();
            return;
        }
        float ease = 1.0f - std::pow(1.0f - t, 3.0f);
        m_rotation = QQuaternion::slerp(m_animStartRot, m_animTargetRot, ease);
        m_cameraTarget = m_animStartTarget + (m_animTargetTarget - m_animStartTarget) * ease;
        m_cameraDistance = m_animStartDist + (m_animTargetDist - m_animStartDist) * ease;
        updateProjectionMatrix();
        update();
    });
}

MasterWidget::~MasterWidget() {
    if (isValid()) {
        makeCurrent();
        std::unordered_set<QOpenGLVertexArrayObject*> destroyedVaos;
        std::unordered_set<QOpenGLBuffer*> destroyedBuffers;
        for (auto& gm : m_glMeshes) {
            if (gm.vao && destroyedVaos.insert(gm.vao.get()).second) {
                if (gm.vao->isCreated()) gm.vao->destroy();
            }
            if (gm.vbo && destroyedBuffers.insert(gm.vbo.get()).second) {
                if (gm.vbo->isCreated()) gm.vbo->destroy();
            }
            if (gm.ibo && destroyedBuffers.insert(gm.ibo.get()).second) {
                if (gm.ibo->isCreated()) gm.ibo->destroy();
            }
        }
        m_glMeshes.clear();
        if (m_gridVbo.isCreated()) {
            m_gridVbo.destroy();
        }
        // 关键修复：确保所有着色器 Program 在当前有效的 OpenGL 上下文内安全析构
        m_program.reset();
        m_lineProgram.reset();
        doneCurrent();
    }
}

void MasterWidget::setLoading(bool loading, const QString& status) {
    if (loading && m_model && !m_model->meshes.isEmpty()) {
        // 模型已在视口中完整呈现，坚决阻止重复加载遮罩覆盖已有视图
        return;
    }
    m_isLoading = loading;
    m_loadingStatus = status;
    if (m_isLoading) {
        m_errorMessage.clear();
        if (!m_animTimer->isActive()) m_animTimer->start(16); // 60 FPS 平滑动画
    } else {
        m_animTimer->stop();
    }
    update();
}

void MasterWidget::setErrorMessage(const QString& msg) {
    m_isLoading = false;
    m_errorMessage = msg;
    if (m_animTimer->isActive()) m_animTimer->stop();
    update();
}

void MasterWidget::setModel(ModelDataPtr model) {
    m_model = model;
    m_errorMessage.clear();
    m_isLoading = false;
    m_loadingStatus.clear();
    if (m_animTimer && m_animTimer->isActive()) {
        m_animTimer->stop();
    }
    if (isValid()) {
        makeCurrent();
        buildBuffers();
        doneCurrent();
    }

    if (m_model) {
        m_cameraTarget = m_model->center;
        m_cameraDistance = m_model->boundingRadius * 2.6f;
        m_rotation = QQuaternion::fromAxisAndAngle(QVector3D(1, 0, 0), -25.0f) *
                     QQuaternion::fromAxisAndAngle(QVector3D(0, 1, 0), 45.0f);
        updateProjectionMatrix();
    }
    setLoading(false);
    update();
}

void MasterWidget::setShadingMode(ShadingMode mode) {
    m_shadingMode = mode;
    update();
}

void MasterWidget::setShowGrid(bool show) {
    m_showGrid = show;
    update();
}

void MasterWidget::setShowAxis(bool show) {
    m_showAxis = show;
    update();
}

void MasterWidget::setShowWireframe(bool show) {
    m_showWireframe = show;
    update();
}

void MasterWidget::setShowFeatureEdges(bool show) {
    m_showFeatureEdges = show;
    update();
}

void MasterWidget::setShowBoundingBox(bool show) {
    m_showBoundingBox = show;
    update();
}

void MasterWidget::setSectionEnabled(bool enabled) {
    m_sectionEnabled = enabled;
    update();
}

void MasterWidget::setSectionAxis(int axis) {
    m_sectionAxis = axis;
    update();
}

void MasterWidget::setSectionDepth(float depthPercent) {
    m_sectionDepthPercent = std::clamp(depthPercent, 0.0f, 1.0f);
    update();
}

void MasterWidget::setOrthographic(bool ortho) {
    m_orthographic = ortho;
    updateProjectionMatrix();
    update();
}

void MasterWidget::setNavigationPreset(NavigationPreset preset) {
    m_navPreset = preset;
}

void MasterWidget::animateCameraTo(const QQuaternion& targetRot, const QVector3D& targetTarget, float targetDist, int durationMs) {
    if (durationMs <= 0) {
        stopCameraAnimation();
        m_rotation = targetRot;
        m_cameraTarget = targetTarget;
        m_cameraDistance = targetDist;
        updateProjectionMatrix();
        update();
        return;
    }
    m_animStartRot = m_rotation;
    m_animTargetRot = targetRot;
    m_animStartTarget = m_cameraTarget;
    m_animTargetTarget = targetTarget;
    m_animStartDist = m_cameraDistance;
    m_animTargetDist = targetDist;
    m_cameraAnimDuration = durationMs;
    m_isCameraAnimating = true;
    m_cameraAnimElapsed.restart();
    if (!m_cameraAnimTimer->isActive()) {
        m_cameraAnimTimer->start(16);
    }
}

void MasterWidget::stopCameraAnimation() {
    m_isCameraAnimating = false;
    if (m_cameraAnimTimer && m_cameraAnimTimer->isActive()) {
        m_cameraAnimTimer->stop();
    }
}

void MasterWidget::fitView() {
    if (m_model) {
        animateCameraTo(m_rotation, m_model->center, m_model->boundingRadius * 2.6f, 220);
    }
}

void MasterWidget::setPartVisible(int partId, bool visible) {
    bool changed = false;
    for (auto& gm : m_glMeshes) {
        if (gm.partId == partId) {
            if (gm.visible != visible) {
                gm.visible = visible;
                changed = true;
            }
        }
    }
    if (changed) {
        update();
    }
}

void MasterWidget::setAllPartsVisible(bool visible) {
    bool changed = false;
    for (auto& gm : m_glMeshes) {
        if (gm.visible != visible) {
            gm.visible = visible;
            changed = true;
        }
    }
    if (changed) {
        update();
    }
}

bool MasterWidget::isPartVisible(int partId) const {
    for (const auto& gm : m_glMeshes) {
        if (gm.partId == partId && !gm.isFeatureEdge) {
            return gm.visible;
        }
    }
    return true;
}

QVector3D MasterWidget::getCursorWorldPointOnFocusPlane(const QPoint& screenPos) {
    int w = width();
    int h = height();
    if (w <= 0 || h <= 0) return m_cameraTarget;

    float x_ndc = (2.0f * screenPos.x()) / static_cast<float>(w) - 1.0f;
    float y_ndc = 1.0f - (2.0f * screenPos.y()) / static_cast<float>(h);

    QMatrix4x4 invVP = (m_projMatrix * m_viewMatrix).inverted();
    QVector4D pNear = invVP * QVector4D(x_ndc, y_ndc, -1.0f, 1.0f);
    QVector4D pFar  = invVP * QVector4D(x_ndc, y_ndc,  1.0f, 1.0f);

    QVector3D rayStart = pNear.toVector3DAffine();
    QVector3D rayEnd   = pFar.toVector3DAffine();
    QVector3D rayDir   = (rayEnd - rayStart).normalized();

    // 焦点平面的法向量即相机视线正向 (Forward)
    QVector3D forward = m_rotation.rotatedVector(QVector3D(0, 0, -1));
    float denom = QVector3D::dotProduct(rayDir, forward);
    if (std::abs(denom) > 1e-5f) {
        float t = QVector3D::dotProduct(m_cameraTarget - rayStart, forward) / denom;
        return rayStart + t * rayDir;
    }
    return m_cameraTarget;
}

void MasterWidget::switchCamera(CameraPreset preset) {
    QQuaternion targetRot = m_rotation;
    QVector3D targetPos = m_cameraTarget;
    float targetDist = m_cameraDistance;

    switch (preset) {
    case CameraPreset::Front:
        targetRot = QQuaternion::fromAxisAndAngle(QVector3D(0, 1, 0), 0.0f);
        break;
    case CameraPreset::Back:
        targetRot = QQuaternion::fromAxisAndAngle(QVector3D(0, 1, 0), 180.0f);
        break;
    case CameraPreset::Left:
        targetRot = QQuaternion::fromAxisAndAngle(QVector3D(0, 1, 0), 90.0f);
        break;
    case CameraPreset::Right:
        targetRot = QQuaternion::fromAxisAndAngle(QVector3D(0, 1, 0), -90.0f);
        break;
    case CameraPreset::Top:
        targetRot = QQuaternion::fromAxisAndAngle(QVector3D(1, 0, 0), 90.0f);
        break;
    case CameraPreset::Bottom:
        targetRot = QQuaternion::fromAxisAndAngle(QVector3D(1, 0, 0), -90.0f);
        break;
    case CameraPreset::Reset:
        if (m_model) {
            targetPos = m_model->center;
            targetDist = m_model->boundingRadius * 2.6f;
        }
        targetRot = QQuaternion::fromAxisAndAngle(QVector3D(1, 0, 0), -25.0f) *
                     QQuaternion::fromAxisAndAngle(QVector3D(0, 1, 0), 45.0f);
        break;
    }
    animateCameraTo(targetRot, targetPos, targetDist, 220);
}

void MasterWidget::initializeGL() {
    initializeOpenGLFunctions();

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    // CAD 模型多包含单面片薄壳结构，关闭单面剔除以保证正反面均清晰可见
    glDisable(GL_CULL_FACE);

    // 启用硬件多重采样抗锯齿
    glEnable(GL_MULTISAMPLE);

    initShaders();

    if (m_model && !m_model->meshes.isEmpty()) {
        buildBuffers();
    }
}

void MasterWidget::initShaders() {
    // 主色彩材质着色器 (双光源 + Blinn-Phong 镜面高光反射模型 + 动态轴向剖切截断)
    m_program = std::make_unique<QOpenGLShaderProgram>();
    const char* vShader = R"(
        attribute vec3 aPos;
        attribute vec3 aNorm;
        attribute vec4 aCol;
        uniform mat4 uMVP;
        uniform mat4 uModel;
        varying vec3 vWorldPos;
        varying vec3 vNorm;
        varying vec4 vColor;
        void main() {
            vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
            vNorm = mat3(uModel) * aNorm;
            vColor = aCol;
            gl_Position = uMVP * vec4(aPos, 1.0);
        }
    )";
    const char* fShader = R"(
        varying vec3 vWorldPos;
        varying vec3 vNorm;
        varying vec4 vColor;
        uniform vec3 uDiffuseColor;
        uniform vec3 uViewPos;
        uniform int uShadingMode; // 0: 材质色, 1: 顶点色, 2: 纯白模, 3: 法线
        uniform int uHasVertexColor; // 1: 当前网格包含顶点属性色彩
        uniform int uSectionEnabled;
        uniform vec4 uSectionPlane; // 点乘法: dot(vWorldPos, uSectionPlane.xyz) + uSectionPlane.w > 0.0 则裁剪

        void main() {
            // 动态轴向剖切深度剔除 (Section View Discard)
            if (uSectionEnabled == 1 && (dot(vWorldPos, uSectionPlane.xyz) + uSectionPlane.w > 0.0)) {
                discard;
            }

            vec3 viewDir = normalize(uViewPos - vWorldPos);
            vec3 norm = normalize(vNorm);
            if (length(norm) < 0.1) norm = vec3(0.0, 1.0, 0.0);
            // 视线自适应双面法线：杜绝 gl_FrontFacing 在缝隙三角形/复杂拓扑微表面上的光栅化抖动与跳变
            if (dot(norm, viewDir) < 0.0) {
                norm = -norm;
            }

            if (uShadingMode == 3) {
                gl_FragColor = vec4(norm * 0.5 + 0.5, 1.0); // 法线直接可视化
                return;
            }

            // 三点工业摄影照明系统 (Key + Fill + Rim)
            // 1. 主平行光 (Key Light，来自右上方，提供主造型明暗与清晰立体感)
            vec3 keyLightDir = normalize(vec3(0.50, 0.80, 0.60));
            // 2. 弱辅助光 (Fill Light，来自左下方，柔和补亮暗部阴影，杜绝死黑与断层)
            vec3 fillLightDir = normalize(vec3(-0.50, -0.30, -0.40));
            // 3. 轮廓光 (Rim Light，来自斜后方，勾勒曲面轮廓边缘反光)
            vec3 rimLightDir = normalize(vec3(0.0, 0.60, -0.80));

            // 标准兰伯特漫反射 (切勿使用 abs(dot)，abs 会使背光面折射反转并产生锐利三角形亮暗杂斑)
            float keyDiff = max(dot(norm, keyLightDir), 0.0);
            float fillDiff = max(dot(norm, fillLightDir), 0.0);
            float rimDiff = max(dot(norm, rimLightDir), 0.0);

            // 视线向量与半角向量 (Blinn-Phong 真实高保真镜面反射)
            float spec = 0.0;
            if (keyDiff > 0.0) {
                vec3 halfVector = normalize(keyLightDir + viewDir);
                float NdotH = max(dot(norm, halfVector), 0.0);
                spec = pow(NdotH, 32.0) * 0.35;
            }

            vec3 baseCol = uDiffuseColor;
            if (uShadingMode == 1 || (uShadingMode == 0 && uHasVertexColor == 1)) {
                baseCol = vColor.rgb;
            } else if (uShadingMode == 2) {
                baseCol = vec3(0.92, 0.92, 0.92); // 纯白结构模
            }

            // 剖切截面内壁/空腔背面强化高对比度，呈现清脆机械剖视截面
            if (uSectionEnabled == 1 && !gl_FrontFacing) {
                baseCol = vec3(0.88, 0.42, 0.28); // 工业剖切截面铜橙警示色
            }

            // 工业 CAD 级别光照配比：环境光 0.30，主光 0.55，辅光 0.25，轮廓光 0.12
            vec3 ambient = 0.30 * baseCol;
            vec3 diffuse = (keyDiff * 0.55 + fillDiff * 0.25 + rimDiff * 0.12) * baseCol;
            vec3 specular = vec3(spec); // 高级工业金属导角反光
            gl_FragColor = vec4(clamp(ambient + diffuse + specular, 0.0, 1.0), 1.0);
        }
    )";
    bool ok1 = m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, vShader) &&
               m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, fShader) &&
               m_program->link();
    if (!ok1) {
        qCritical() << "[MasterWidget] Main shader program link FAILED:" << m_program->log();
    }

    // 辅助网格、线框与 CAD 特征硬轮廓线着色器
    m_lineProgram = std::make_unique<QOpenGLShaderProgram>();
    const char* lineVShader = R"(
        attribute vec3 aPos;
        uniform mat4 uMVP;
        uniform mat4 uModel;
        varying vec3 vWorldPos;
        void main() {
            vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
            gl_Position = uMVP * vec4(aPos, 1.0);
        }
    )";
    const char* lineFShader = R"(
        varying vec3 vWorldPos;
        uniform vec4 uLineColor;
        uniform int uSectionEnabled;
        uniform vec4 uSectionPlane;
        void main() {
            if (uSectionEnabled == 1 && (dot(vWorldPos, uSectionPlane.xyz) + uSectionPlane.w > 0.0)) {
                discard;
            }
            gl_FragColor = uLineColor;
        }
    )";
    bool ok2 = m_lineProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, lineVShader) &&
               m_lineProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, lineFShader) &&
               m_lineProgram->link();
    if (!ok2) {
        qCritical() << "[MasterWidget] Line shader program link FAILED:" << m_lineProgram->log();
    }

    m_shadersValid = (ok1 && ok2);
    if (!m_shadersValid) {
        setErrorMessage("OpenGL 着色器编译或链接失败，请检查显卡驱动环境");
    }
}

void MasterWidget::updateGridBuffer() {
    float extent = m_model ? m_model->boundingRadius * 2.0f : 10.0f;
    float step = extent / 10.0f;
    float y = m_model ? m_model->boundsMin.y() : 0.0f;
    float cx = m_model ? m_model->center.x() : 0.0f;
    float cz = m_model ? m_model->center.z() : 0.0f;

    QVector<QVector3D> lines;
    lines.reserve(44);
    for (float i = -extent; i <= extent; i += step) {
        lines.push_back(QVector3D(cx + i, y, cz - extent));
        lines.push_back(QVector3D(cx + i, y, cz + extent));
        lines.push_back(QVector3D(cx - extent, y, cz + i));
        lines.push_back(QVector3D(cx + extent, y, cz + i));
    }

    if (!m_gridVbo.isCreated()) {
        m_gridVbo.create();
    }
    m_gridVbo.bind();
    m_gridVbo.allocate(lines.constData(), lines.size() * sizeof(QVector3D));
    m_gridVbo.release();
    m_gridVertexCount = lines.size();
}

// 6 视锥体平面快速裁剪结构体 (Gribb-Hartmann 快速提取与 AABB 快速相交)
struct FrustumPlanes {
    QVector4D planes[6]; // Left, Right, Bottom, Top, Near, Far (朝内为正)

    static FrustumPlanes fromMatrix(const QMatrix4x4& m) {
        FrustumPlanes fp;
        const QVector4D r0 = m.row(0);
        const QVector4D r1 = m.row(1);
        const QVector4D r2 = m.row(2);
        const QVector4D r3 = m.row(3);

        fp.planes[0] = r3 + r0; // Left
        fp.planes[1] = r3 - r0; // Right
        fp.planes[2] = r3 + r1; // Bottom
        fp.planes[3] = r3 - r1; // Top
        fp.planes[4] = r3 + r2; // Near
        fp.planes[5] = r3 - r2; // Far

        for (int i = 0; i < 6; ++i) {
            float len = std::sqrt(fp.planes[i].x() * fp.planes[i].x() +
                                  fp.planes[i].y() * fp.planes[i].y() +
                                  fp.planes[i].z() * fp.planes[i].z());
            if (len > 1e-6f) {
                fp.planes[i] /= len;
            }
        }
        return fp;
    }

    bool isBoxVisible(const QVector3D& bmin, const QVector3D& bmax) const {
        for (int i = 0; i < 6; ++i) {
            const QVector4D& p = planes[i];
            float px = (p.x() > 0.0f) ? bmax.x() : bmin.x();
            float py = (p.y() > 0.0f) ? bmax.y() : bmin.y();
            float pz = (p.z() > 0.0f) ? bmax.z() : bmin.z();
            if (p.x() * px + p.y() * py + p.z() * pz + p.w() < 0.0f) {
                return false;
            }
        }
        return true;
    }
};

void MasterWidget::buildBuffers() {
    std::unordered_set<QOpenGLVertexArrayObject*> destroyedVaos;
    std::unordered_set<QOpenGLBuffer*> destroyedBuffers;
    for (auto& gm : m_glMeshes) {
        if (gm.vao && destroyedVaos.insert(gm.vao.get()).second) {
            if (gm.vao->isCreated()) gm.vao->destroy();
        }
        if (gm.vbo && destroyedBuffers.insert(gm.vbo.get()).second) {
            if (gm.vbo->isCreated()) gm.vbo->destroy();
        }
        if (gm.ibo && destroyedBuffers.insert(gm.ibo.get()).second) {
            if (gm.ibo->isCreated()) gm.ibo->destroy();
        }
    }
    m_glMeshes.clear();

    if (!m_shadersValid) {
        return;
    }

    if (!m_model) {
        updateGridBuffer();
        return;
    }

    if (m_program) {
        m_program->bind();
    }

    struct ProtoGpuRes {
        std::shared_ptr<QOpenGLVertexArrayObject> vao;
        std::shared_ptr<QOpenGLBuffer> vbo;
        std::shared_ptr<QOpenGLBuffer> ibo;
        int indexCount = 0;
        bool isCompact = false;
    };
    QMap<int, ProtoGpuRes> protoCache;

    for (const auto& mesh : m_model->meshes) {
        if (mesh.vertices.isEmpty() || mesh.indices.isEmpty()) continue;

        GLMesh gm;
        gm.diffuseColor = mesh.diffuseColor;
        gm.hasVertexColor = mesh.hasVertexColors;
        gm.indexCount = mesh.indices.size();
        gm.primitiveType = mesh.primitiveType;
        gm.lineWidth = mesh.lineWidth;
        gm.isFeatureEdge = mesh.isFeatureEdge;
        gm.visible = mesh.visible;
        gm.partId = mesh.partId;
        gm.transform = mesh.transform;
        gm.prototypeId = mesh.prototypeId;

        // 8 角点矩阵快速映射计算世界 AABB (供每帧视锥体剔除快速相交)
        if (mesh.hasLocalAabb) {
            const bool hasTrsf = !mesh.transform.isIdentity();
            if (!hasTrsf) {
                gm.worldAabbMin = mesh.localAabbMin;
                gm.worldAabbMax = mesh.localAabbMax;
            } else {
                const QVector3D& mn = mesh.localAabbMin;
                const QVector3D& mx = mesh.localAabbMax;
                const QVector3D corners[8] = {
                    mesh.transform.map(QVector3D(mn.x(), mn.y(), mn.z())),
                    mesh.transform.map(QVector3D(mx.x(), mn.y(), mn.z())),
                    mesh.transform.map(QVector3D(mn.x(), mx.y(), mn.z())),
                    mesh.transform.map(QVector3D(mx.x(), mx.y(), mn.z())),
                    mesh.transform.map(QVector3D(mn.x(), mn.y(), mx.z())),
                    mesh.transform.map(QVector3D(mx.x(), mn.y(), mx.z())),
                    mesh.transform.map(QVector3D(mn.x(), mx.y(), mx.z())),
                    mesh.transform.map(QVector3D(mx.x(), mx.y(), mx.z()))
                };
                gm.worldAabbMin = QVector3D(1e9f, 1e9f, 1e9f);
                gm.worldAabbMax = QVector3D(-1e9f, -1e9f, -1e9f);
                for (int cIdx = 0; cIdx < 8; ++cIdx) {
                    gm.worldAabbMin.setX(std::min(gm.worldAabbMin.x(), corners[cIdx].x()));
                    gm.worldAabbMin.setY(std::min(gm.worldAabbMin.y(), corners[cIdx].y()));
                    gm.worldAabbMin.setZ(std::min(gm.worldAabbMin.z(), corners[cIdx].z()));
                    gm.worldAabbMax.setX(std::max(gm.worldAabbMax.x(), corners[cIdx].x()));
                    gm.worldAabbMax.setY(std::max(gm.worldAabbMax.y(), corners[cIdx].y()));
                    gm.worldAabbMax.setZ(std::max(gm.worldAabbMax.z(), corners[cIdx].z()));
                }
            }
            gm.hasWorldAabb = true;
        }

        // 原型实例化快速复用：按 (prototypeId, isFeatureEdge) 复合键区分实体与特征棱线，杜绝缓存键碰撞
        int protoKey = (mesh.prototypeId << 2) | (mesh.isFeatureEdge ? 1 : 0);
        if (mesh.prototypeId >= 0 && protoCache.contains(protoKey)) {
            const auto& proto = protoCache[protoKey];
            gm.vao = proto.vao;
            gm.vbo = proto.vbo;
            gm.ibo = proto.ibo;
            gm.indexCount = proto.indexCount;
            gm.isCompact = proto.isCompact;
            m_glMeshes.push_back(gm);
            continue;
        }

        gm.vao = std::make_shared<QOpenGLVertexArrayObject>();
        gm.vao->create();
        gm.vao->bind();

        gm.vbo = std::make_shared<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
        gm.vbo->create();
        gm.vbo->bind();

        gm.ibo = std::make_shared<QOpenGLBuffer>(QOpenGLBuffer::IndexBuffer);
        gm.ibo->create();
        gm.ibo->bind();

        if (gm.isFeatureEdge || gm.primitiveType == 0x0001) {
            if (m_lineProgram) m_lineProgram->bind();
            gm.vbo->allocate(mesh.vertices.constData(), mesh.vertices.size() * sizeof(Vertex));
            gm.ibo->allocate(mesh.indices.constData(), mesh.indices.size() * sizeof(uint32_t));

            if (m_lineProgram) {
                int posAttr = m_lineProgram->attributeLocation("aPos");
                if (posAttr >= 0) {
                    m_lineProgram->enableAttributeArray(posAttr);
                    m_lineProgram->setAttributeBuffer(posAttr, GL_FLOAT, offsetof(Vertex, position), 3, sizeof(Vertex));
                }
            }
        } else {
            gm.isCompact = !mesh.hasVertexColors;
            if (gm.isCompact) {
                QVector<CompactVertex> compactVerts;
                compactVerts.resize(mesh.vertices.size());
                for (int i = 0; i < mesh.vertices.size(); ++i) {
                    compactVerts[i].position = mesh.vertices[i].position;
                    compactVerts[i].normal = mesh.vertices[i].normal;
                }
                gm.vbo->allocate(compactVerts.constData(), compactVerts.size() * sizeof(CompactVertex));
            } else {
                gm.vbo->allocate(mesh.vertices.constData(), mesh.vertices.size() * sizeof(Vertex));
            }

            gm.ibo->allocate(mesh.indices.constData(), mesh.indices.size() * sizeof(uint32_t));

            if (m_program) {
                int posAttr = m_program->attributeLocation("aPos");
                int normAttr = m_program->attributeLocation("aNorm");
                int colAttr = m_program->attributeLocation("aCol");

                if (gm.isCompact) {
                    if (posAttr >= 0) {
                        m_program->enableAttributeArray(posAttr);
                        m_program->setAttributeBuffer(posAttr, GL_FLOAT, offsetof(CompactVertex, position), 3, sizeof(CompactVertex));
                    }
                    if (normAttr >= 0) {
                        m_program->enableAttributeArray(normAttr);
                        m_program->setAttributeBuffer(normAttr, GL_FLOAT, offsetof(CompactVertex, normal), 3, sizeof(CompactVertex));
                    }
                    if (colAttr >= 0) {
                        m_program->disableAttributeArray(colAttr);
                        m_program->setAttributeValue(colAttr, QVector4D(1.0f, 1.0f, 1.0f, 1.0f));
                    }
                } else {
                    if (posAttr >= 0) {
                        m_program->enableAttributeArray(posAttr);
                        m_program->setAttributeBuffer(posAttr, GL_FLOAT, offsetof(Vertex, position), 3, sizeof(Vertex));
                    }
                    if (normAttr >= 0) {
                        m_program->enableAttributeArray(normAttr);
                        m_program->setAttributeBuffer(normAttr, GL_FLOAT, offsetof(Vertex, normal), 3, sizeof(Vertex));
                    }
                    if (colAttr >= 0) {
                        m_program->enableAttributeArray(colAttr);
                        m_program->setAttributeBuffer(colAttr, GL_FLOAT, offsetof(Vertex, color), 4, sizeof(Vertex));
                    }
                }
            }
        }

        gm.vao->release();
        gm.vbo->release();
        gm.ibo->release();

        if (mesh.prototypeId >= 0) {
            protoCache[protoKey] = { gm.vao, gm.vbo, gm.ibo, gm.indexCount, gm.isCompact };
        }

        m_glMeshes.push_back(gm);
    }

    if (m_program) {
        m_program->release();
    }

    updateGridBuffer();
}

float MasterWidget::minCameraDistance() const {
    if (m_model && m_model->boundingRadius > 1e-4f) {
        return std::max(0.001f, m_model->boundingRadius * 0.005f);
    }
    return 0.01f;
}

float MasterWidget::maxCameraDistance() const {
    if (m_model && m_model->boundingRadius > 1e-4f) {
        return std::max(100000.0f, m_model->boundingRadius * 50.0f);
    }
    return 100000.0f;
}

void MasterWidget::updateProjectionMatrix() {
    int w = width();
    int h = height();
    if (w <= 0 || h <= 0) return;
    float aspect = static_cast<float>(w) / std::max(1, h);

    m_projMatrix.setToIdentity();
    float nearPlane = std::max(0.01f, m_cameraDistance * 0.005f);
    float farPlane = std::max(500.0f, m_cameraDistance * 50.0f);
    if (m_model && m_model->boundingRadius > 1e-4f) {
        farPlane = std::max(farPlane, (m_cameraDistance + m_model->boundingRadius) * 2.0f);
    }

    if (m_orthographic) {
        float r = m_cameraDistance * 0.5f;
        m_projMatrix.ortho(-r * aspect, r * aspect, -r, r, nearPlane, farPlane);
    } else {
        m_projMatrix.perspective(45.0f, aspect, nearPlane, farPlane);
    }
}

void MasterWidget::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
    updateProjectionMatrix();
}

void MasterWidget::paintGL() {
    glClearColor(0.10f, 0.12f, 0.16f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (!m_shadersValid) {
        return; // 着色器未就绪或链接失败时安全熔断
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);

    // 动态同步投影 Frustum 矩阵，杜绝大尺度 CAD 实体被远裁剪面剔除
    updateProjectionMatrix();

    // 构建 View 矩阵
    m_viewMatrix.setToIdentity();
    m_viewMatrix.translate(0, 0, -m_cameraDistance);
    m_viewMatrix.rotate(m_rotation);
    m_viewMatrix.translate(-m_cameraTarget);

    QMatrix4x4 modelMatrix;
    modelMatrix.setToIdentity();
    QMatrix4x4 mvp = m_projMatrix * m_viewMatrix * modelMatrix;

    // 计算动态剖切平面的空间代数方程 (Ax + By + Cz + D = 0)
    QVector4D sectionPlane(0.0f, 1.0f, 0.0f, 0.0f);
    if (m_model) {
        float minVal = 0.0f, maxVal = 0.0f;
        if (m_sectionAxis == 0) { // X 轴 (YZ 剖切面)
            minVal = m_model->boundsMin.x();
            maxVal = m_model->boundsMax.x();
            float cutPos = minVal + m_sectionDepthPercent * (maxVal - minVal);
            sectionPlane = QVector4D(1.0f, 0.0f, 0.0f, -cutPos);
        } else if (m_sectionAxis == 2) { // Z 轴 (XY 剖切面)
            minVal = m_model->boundsMin.z();
            maxVal = m_model->boundsMax.z();
            float cutPos = minVal + m_sectionDepthPercent * (maxVal - minVal);
            sectionPlane = QVector4D(0.0f, 0.0f, 1.0f, -cutPos);
        } else { // 默认 Y 轴 (XZ 剖切面)
            minVal = m_model->boundsMin.y();
            maxVal = m_model->boundsMax.y();
            float cutPos = minVal + m_sectionDepthPercent * (maxVal - minVal);
            sectionPlane = QVector4D(0.0f, 1.0f, 0.0f, -cutPos);
        }
    }

    // 1. 绘制辅助地面网格
    if (m_showGrid) {
        drawGrid();
    }

    // 延迟构建 GPU 缓冲 (防御 initializeGL 阶段尚未载入模型的情形)
    if (m_glMeshes.isEmpty() && m_model && !m_model->meshes.isEmpty()) {
        buildBuffers();
    }

    // 2. 绘制三维模型本体 (VAO 快速绑定渲染)
    if (m_program && !m_glMeshes.isEmpty()) {
        if (m_showWireframe) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        }

        m_program->bind();
        m_program->setUniformValue("uMVP", mvp);
        m_program->setUniformValue("uModel", modelMatrix);
        m_program->setUniformValue("uShadingMode", static_cast<int>(m_shadingMode));
        m_program->setUniformValue("uSectionEnabled", m_sectionEnabled ? 1 : 0);
        m_program->setUniformValue("uSectionPlane", sectionPlane);

        QVector3D cameraPos = m_cameraTarget + m_rotation.inverted().rotatedVector(QVector3D(0, 0, m_cameraDistance));
        m_program->setUniformValue("uViewPos", cameraPos);

        // 为消除实体三角面与特征线/线框间的共面 Z-fighting 闪烁，绘制实体三角面时启用 PolygonOffset 将面压后
        if (m_showFeatureEdges || m_showWireframe) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.0f, 1.0f);
        }

        FrustumPlanes frustum = FrustumPlanes::fromMatrix(mvp);
        for (auto& gm : m_glMeshes) {
            if (!gm.visible) continue;
            if (gm.primitiveType == 0x0001 || gm.isFeatureEdge) continue;
            // 视锥体快速裁剪：剔除视口外部构件，0 次 Draw Call
            if (gm.hasWorldAabb && !frustum.isBoxVisible(gm.worldAabbMin, gm.worldAabbMax)) continue; // 线段留给线着色器绘制

            const bool hasInstTrsf = !gm.transform.isIdentity();
            QMatrix4x4 curModel = hasInstTrsf ? (modelMatrix * gm.transform) : modelMatrix;
            QMatrix4x4 curMVP = hasInstTrsf ? (mvp * gm.transform) : mvp;
            m_program->setUniformValue("uModel", curModel);
            m_program->setUniformValue("uMVP", curMVP);

            m_program->setUniformValue("uDiffuseColor", gm.diffuseColor);
            m_program->setUniformValue("uHasVertexColor", gm.hasVertexColor ? 1 : 0);

            if (gm.vao && gm.vao->isCreated()) {
                gm.vao->bind();
            } else {
                if (gm.vbo) gm.vbo->bind();
                if (gm.ibo) gm.ibo->bind();
                int posAttr = m_program->attributeLocation("aPos");
                int normAttr = m_program->attributeLocation("aNorm");
                int colAttr = m_program->attributeLocation("aCol");

                if (gm.isCompact) {
                    if (posAttr >= 0) {
                        m_program->enableAttributeArray(posAttr);
                        m_program->setAttributeBuffer(posAttr, GL_FLOAT, offsetof(CompactVertex, position), 3, sizeof(CompactVertex));
                    }
                    if (normAttr >= 0) {
                        m_program->enableAttributeArray(normAttr);
                        m_program->setAttributeBuffer(normAttr, GL_FLOAT, offsetof(CompactVertex, normal), 3, sizeof(CompactVertex));
                    }
                    if (colAttr >= 0) {
                        m_program->disableAttributeArray(colAttr);
                        m_program->setAttributeValue(colAttr, QVector4D(1.0f, 1.0f, 1.0f, 1.0f));
                    }
                } else {
                    if (posAttr >= 0) {
                        m_program->enableAttributeArray(posAttr);
                        m_program->setAttributeBuffer(posAttr, GL_FLOAT, offsetof(Vertex, position), 3, sizeof(Vertex));
                    }
                    if (normAttr >= 0) {
                        m_program->enableAttributeArray(normAttr);
                        m_program->setAttributeBuffer(normAttr, GL_FLOAT, offsetof(Vertex, normal), 3, sizeof(Vertex));
                    }
                    if (colAttr >= 0) {
                        m_program->enableAttributeArray(colAttr);
                        m_program->setAttributeBuffer(colAttr, GL_FLOAT, offsetof(Vertex, color), 4, sizeof(Vertex));
                    }
                }
            }

            if (gm.primitiveType == 0x0000) { // GL_POINTS
                glPointSize(3.0f);
                glDrawElements(GL_POINTS, gm.indexCount, GL_UNSIGNED_INT, nullptr);
            } else {
                glDrawElements(GL_TRIANGLES, gm.indexCount, GL_UNSIGNED_INT, nullptr);
            }

            if (gm.vao && gm.vao->isCreated()) {
                gm.vao->release();
            } else {
                if (gm.vbo) gm.vbo->release();
                if (gm.ibo) gm.ibo->release();
            }
        }

        if (m_showFeatureEdges || m_showWireframe) {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }

        m_program->release();

        if (m_showWireframe) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }
    }

    // 2.2 绘制 CAD 原生特征硬轮廓线 (Shaded with Edges)
    if (m_lineProgram && m_showFeatureEdges && !m_glMeshes.isEmpty()) {
        m_lineProgram->bind();
        m_lineProgram->setUniformValue("uMVP", mvp);
        m_lineProgram->setUniformValue("uModel", modelMatrix);
        m_lineProgram->setUniformValue("uSectionEnabled", m_sectionEnabled ? 1 : 0);
        m_lineProgram->setUniformValue("uSectionPlane", sectionPlane);

        FrustumPlanes edgeFrustum = FrustumPlanes::fromMatrix(mvp);
        for (auto& gm : m_glMeshes) {
            if (!gm.visible) continue;
            if (gm.primitiveType != 0x0001 && !gm.isFeatureEdge) continue;
            // 视锥体快速裁剪：剔除视口外部特征线
            if (gm.hasWorldAabb && !edgeFrustum.isBoxVisible(gm.worldAabbMin, gm.worldAabbMax)) continue;

            const bool hasInstTrsf = !gm.transform.isIdentity();
            QMatrix4x4 curModel = hasInstTrsf ? (modelMatrix * gm.transform) : modelMatrix;
            QMatrix4x4 curMVP = hasInstTrsf ? (mvp * gm.transform) : mvp;
            m_lineProgram->setUniformValue("uModel", curModel);
            m_lineProgram->setUniformValue("uMVP", curMVP);

            // 特征棱线使用深空炭黑色 (Dark Charcoal CAD Line)，增强工业结构几何质感
            m_lineProgram->setUniformValue("uLineColor", QVector4D(gm.diffuseColor, 1.0f));

            if (gm.vao && gm.vao->isCreated()) {
                gm.vao->bind();
            } else {
                if (gm.vbo) gm.vbo->bind();
                if (gm.ibo) gm.ibo->bind();
                int posAttr = m_lineProgram->attributeLocation("aPos");
                if (posAttr >= 0) {
                    m_lineProgram->enableAttributeArray(posAttr);
                    m_lineProgram->setAttributeBuffer(posAttr, GL_FLOAT, offsetof(Vertex, position), 3, sizeof(Vertex));
                }
            }

            glLineWidth(gm.lineWidth);
            glDrawElements(GL_LINES, gm.indexCount, GL_UNSIGNED_INT, nullptr);

            if (gm.vao && gm.vao->isCreated()) {
                gm.vao->release();
            } else {
                if (gm.vbo) gm.vbo->release();
                if (gm.ibo) gm.ibo->release();
            }
        }

        m_lineProgram->release();
    }

    // 2.3 绘制三维尺寸包围盒 (Bounding Box 3D Wireframe)
    if (m_showBoundingBox && m_model) {
        drawBoundingBox();
    }

    // 3. 绘制视口坐标轴
    if (m_showAxis) {
        drawAxis();
    }
}

void MasterWidget::paintEvent(QPaintEvent* event) {
    // 先由 QOpenGLWidget 完成底层 OpenGL 3D 渲染流程
    QOpenGLWidget::paintEvent(event);

    // 在标准 paintEvent 绘制管道中启动 QPainter 叠加 2D 尺寸标注与状态遮罩
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    if (m_showBoundingBox && m_model) {
        drawBoundingBoxDimensions(painter);
    }

    if (m_isLoading && (!m_model || m_model->meshes.isEmpty())) {
        drawLoadingSpinner(painter);
    } else if (!m_errorMessage.isEmpty() && (!m_model || m_model->meshes.isEmpty())) {
        drawErrorMessage(painter);
    }
}

void MasterWidget::drawGrid() {
    if (!m_lineProgram) return;
    if (m_gridVertexCount == 0 || !m_gridVbo.isCreated()) {
        updateGridBuffer();
    }
    if (m_gridVertexCount == 0) return;

    m_lineProgram->bind();
    QMatrix4x4 modelMatrix;
    modelMatrix.setToIdentity();
    QMatrix4x4 mvp = m_projMatrix * m_viewMatrix * modelMatrix;
    m_lineProgram->setUniformValue("uMVP", mvp);
    m_lineProgram->setUniformValue("uModel", modelMatrix);
    m_lineProgram->setUniformValue("uSectionEnabled", 0);
    m_lineProgram->setUniformValue("uLineColor", QVector4D(0.22f, 0.28f, 0.38f, 0.8f));

    glDepthMask(GL_FALSE);
    m_gridVbo.bind();
    int posAttr = m_lineProgram->attributeLocation("aPos");
    if (posAttr >= 0) {
        m_lineProgram->enableAttributeArray(posAttr);
        m_lineProgram->setAttributeBuffer(posAttr, GL_FLOAT, 0, 3, sizeof(QVector3D));
        glDrawArrays(GL_LINES, 0, m_gridVertexCount);
        m_lineProgram->disableAttributeArray(posAttr);
    }
    m_gridVbo.release();
    glDepthMask(GL_TRUE);
    m_lineProgram->release();
}

void MasterWidget::drawAxis() {
    if (!m_lineProgram) return;

    // 视口左下角微型坐标系
    glDisable(GL_DEPTH_TEST);
    m_lineProgram->bind();

    QMatrix4x4 axisProj;
    axisProj.ortho(0, width(), 0, height(), -100, 100);
    QMatrix4x4 axisView;
    axisView.translate(50, 50, 0);
    axisView.rotate(m_rotation);

    QMatrix4x4 identityModel;
    identityModel.setToIdentity();
    m_lineProgram->setUniformValue("uMVP", axisProj * axisView);
    m_lineProgram->setUniformValue("uModel", identityModel);
    m_lineProgram->setUniformValue("uSectionEnabled", 0);

    float len = 35.0f;
    QVector3D xLine[] = { QVector3D(0, 0, 0), QVector3D(len, 0, 0) };
    QVector3D yLine[] = { QVector3D(0, 0, 0), QVector3D(0, len, 0) };
    QVector3D zLine[] = { QVector3D(0, 0, 0), QVector3D(0, 0, len) };

    int posAttr = m_lineProgram->attributeLocation("aPos");
    if (posAttr >= 0) {
        m_lineProgram->enableAttributeArray(posAttr);

        // X轴 红色
        m_lineProgram->setUniformValue("uLineColor", QVector4D(0.95f, 0.25f, 0.25f, 1.0f));
        m_lineProgram->setAttributeArray(posAttr, xLine);
        glDrawArrays(GL_LINES, 0, 2);

        // Y轴 绿色
        m_lineProgram->setUniformValue("uLineColor", QVector4D(0.25f, 0.85f, 0.35f, 1.0f));
        m_lineProgram->setAttributeArray(posAttr, yLine);
        glDrawArrays(GL_LINES, 0, 2);

        // Z轴 蓝色
        m_lineProgram->setUniformValue("uLineColor", QVector4D(0.25f, 0.55f, 0.95f, 1.0f));
        m_lineProgram->setAttributeArray(posAttr, zLine);
        glDrawArrays(GL_LINES, 0, 2);

        m_lineProgram->disableAttributeArray(posAttr);
    }
    m_lineProgram->release();
    glEnable(GL_DEPTH_TEST);
}

void MasterWidget::drawBoundingBox() {
    if (!m_lineProgram || !m_model) return;

    QVector3D bMin = m_model->boundsMin;
    QVector3D bMax = m_model->boundsMax;

    QVector3D c[8] = {
        QVector3D(bMin.x(), bMin.y(), bMin.z()), // 0
        QVector3D(bMax.x(), bMin.y(), bMin.z()), // 1
        QVector3D(bMax.x(), bMax.y(), bMin.z()), // 2
        QVector3D(bMin.x(), bMax.y(), bMin.z()), // 3
        QVector3D(bMin.x(), bMin.y(), bMax.z()), // 4
        QVector3D(bMax.x(), bMin.y(), bMax.z()), // 5
        QVector3D(bMax.x(), bMax.y(), bMax.z()), // 6
        QVector3D(bMin.x(), bMax.y(), bMax.z())  // 7
    };

    QVector3D boxLines[24] = {
        c[0], c[1],  c[1], c[2],  c[2], c[3],  c[3], c[0], // bottom
        c[4], c[5],  c[5], c[6],  c[6], c[7],  c[7], c[4], // top
        c[0], c[4],  c[1], c[5],  c[2], c[6],  c[3], c[7]  // vertical pillars
    };

    m_lineProgram->bind();
    QMatrix4x4 modelMatrix;
    modelMatrix.setToIdentity();
    QMatrix4x4 mvp = m_projMatrix * m_viewMatrix * modelMatrix;
    m_lineProgram->setUniformValue("uMVP", mvp);
    m_lineProgram->setUniformValue("uModel", modelMatrix);
    m_lineProgram->setUniformValue("uSectionEnabled", 0);
    // 高级青色三维包围盒线框
    m_lineProgram->setUniformValue("uLineColor", QVector4D(0.20f, 0.75f, 1.0f, 0.85f));

    int posAttr = m_lineProgram->attributeLocation("aPos");
    if (posAttr >= 0) {
        m_lineProgram->enableAttributeArray(posAttr);
        m_lineProgram->setAttributeArray(posAttr, boxLines);
        glLineWidth(1.8f);
        glDrawArrays(GL_LINES, 0, 24);
        m_lineProgram->disableAttributeArray(posAttr);
    }
    m_lineProgram->release();
}

void MasterWidget::drawBoundingBoxDimensions(QPainter& painter) {
    if (!m_model) return;

    QVector3D bMin = m_model->boundsMin;
    QVector3D bMax = m_model->boundsMax;

    float dx = std::abs(bMax.x() - bMin.x());
    float dy = std::abs(bMax.y() - bMin.y());
    float dz = std::abs(bMax.z() - bMin.z());

    QMatrix4x4 vp = m_projMatrix * m_viewMatrix;
    int w = width();
    int h = height();

    auto projectToScreen = [&](const QVector3D& worldPos, QPointF& screenPos) -> bool {
        QVector4D clip = vp * QVector4D(worldPos, 1.0f);
        if (clip.w() <= 0.001f) return false;
        float ndcX = clip.x() / clip.w();
        float ndcY = clip.y() / clip.w();
        if (ndcX < -1.2f || ndcX > 1.2f || ndcY < -1.2f || ndcY > 1.2f) return false;
        screenPos.setX((ndcX + 1.0f) * 0.5f * w);
        screenPos.setY((1.0f - ndcY) * 0.5f * h);
        return true;
    };

    auto drawDimTag = [&](const QVector3D& worldPos, const QString& text, const QColor& accentColor) {
        QPointF pt;
        if (!projectToScreen(worldPos, pt)) return;

        QFont font = painter.font();
        font.setPointSize(9);
        font.setBold(true);
        painter.setFont(font);

        QFontMetrics fm(font);
        int textW = fm.horizontalAdvance(text);
        int tagW = textW + 16;
        int tagH = 20;

        QRectF tagRect(pt.x() - tagW / 2.0, pt.y() - tagH / 2.0, tagW, tagH);

        painter.setPen(QPen(accentColor, 1.2));
        painter.setBrush(QColor(15, 23, 42, 210));
        painter.drawRoundedRect(tagRect, 4, 4);

        painter.setPen(QColor(240, 245, 255));
        painter.drawText(tagRect, Qt::AlignCenter, text);
    };

    // X 轴中点 (红调)
    drawDimTag(QVector3D((bMin.x() + bMax.x()) * 0.5f, bMin.y(), bMin.z()),
               QString("X: %1 mm").arg(dx, 0, 'f', 2), QColor(244, 63, 94));
    // Y 轴中点 (绿调)
    drawDimTag(QVector3D(bMax.x(), (bMin.y() + bMax.y()) * 0.5f, bMin.z()),
               QString("Y: %1 mm").arg(dy, 0, 'f', 2), QColor(34, 197, 94));
    // Z 轴中点 (蓝调)
    drawDimTag(QVector3D(bMax.x(), bMin.y(), (bMin.z() + bMax.z()) * 0.5f),
               QString("Z: %1 mm").arg(dz, 0, 'f', 2), QColor(56, 189, 248));

    // 左下角常驻半透明工业尺寸信息 HUD 卡片
    int hudW = 280;
    int hudH = 32;
    int hudX = 14;
    int hudY = height() - 44;
    QRect hudRect(hudX, hudY, hudW, hudH);

    painter.setPen(QPen(QColor(56, 189, 248, 120), 1));
    painter.setBrush(QColor(15, 23, 42, 220));
    painter.drawRoundedRect(hudRect, 6, 6);

    QFont hudFont = painter.font();
    hudFont.setPointSize(9);
    hudFont.setBold(true);
    painter.setFont(hudFont);

    painter.setPen(QColor(56, 189, 248));
    painter.drawText(QRect(hudX + 8, hudY, 30, hudH), Qt::AlignVCenter | Qt::AlignLeft, "BOX");

    hudFont.setBold(false);
    painter.setFont(hudFont);
    painter.setPen(QColor(220, 230, 245));
    QString hudText = QString("%1 × %2 × %3 mm")
                          .arg(dx, 0, 'f', 1)
                          .arg(dy, 0, 'f', 1)
                          .arg(dz, 0, 'f', 1);
    painter.drawText(QRect(hudX + 40, hudY, hudW - 48, hudH), Qt::AlignVCenter | Qt::AlignLeft, hudText);
}

void MasterWidget::drawLoadingSpinner(QPainter& painter) {
    // 半透明背景遮罩
    painter.fillRect(rect(), QColor(11, 15, 25, 180));

    int cx = width() / 2;
    int cy = height() / 2 - 20;
    int radius = 32;

    // 绘制现代动态加载环
    painter.save();
    painter.translate(cx, cy);
    painter.rotate(m_spinnerAngle);

    QPen trackPen(QColor(40, 55, 80), 4);
    painter.setPen(trackPen);
    painter.drawEllipse(QPointF(0, 0), radius, radius);

    QPen activePen(QColor(56, 189, 248), 4, Qt::SolidLine, Qt::RoundCap);
    painter.setPen(activePen);
    painter.drawArc(QRectF(-radius, -radius, radius * 2, radius * 2), 0, 110 * 16);
    painter.restore();

    // 状态文字与取消提示
    painter.setPen(QColor(240, 245, 255));
    QFont font = painter.font();
    font.setPointSize(12);
    font.setBold(true);
    painter.setFont(font);

    QString text = m_loadingStatus.isEmpty() ? "正在异步解析 3D 模型..." : m_loadingStatus;
    painter.drawText(QRect(0, cy + 45, width(), 30), Qt::AlignCenter, text);

    font.setPointSize(10);
    font.setBold(false);
    painter.setFont(font);
    painter.setPen(QColor(150, 165, 185));
    painter.drawText(QRect(0, cy + 75, width(), 25), Qt::AlignCenter, "按 Esc 键可立即中止加载");
}

void MasterWidget::drawErrorMessage(QPainter& painter) {
    // 暗调半透明提示底板
    painter.fillRect(rect(), QColor(15, 18, 26, 210));

    int cx = width() / 2;
    int cy = height() / 2 - 30;

    // 错误警告图标
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(239, 68, 68, 35)); // 柔和红底
    painter.drawEllipse(QPoint(cx, cy), 32, 32);

    painter.setPen(QPen(QColor(239, 68, 68), 2.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QPoint(cx, cy), 32, 32);

    QFont iconFont = painter.font();
    iconFont.setPointSize(24);
    iconFont.setBold(true);
    painter.setFont(iconFont);
    painter.setPen(QColor(239, 68, 68));
    painter.drawText(QRect(cx - 32, cy - 32, 64, 64), Qt::AlignCenter, "!");

    // 标题
    QFont titleFont = painter.font();
    titleFont.setPointSize(13);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(QColor(245, 247, 250));
    painter.drawText(QRect(20, cy + 45, width() - 40, 28), Qt::AlignCenter, "模型加载提示");

    // 详细信息 (支持自适应多行 CAD 专业指导面板)
    QFont msgFont = painter.font();
    msgFont.setPointSize(10);
    msgFont.setBold(false);
    painter.setFont(msgFont);
    painter.setPen(QColor(170, 185, 205));
    int boxW = std::min(width() - 80, 680);
    QRect textRect(width() / 2 - boxW / 2, cy + 80, boxW, 160);
    painter.drawText(textRect, Qt::AlignTop | Qt::AlignHCenter | Qt::TextWordWrap, m_errorMessage);
}

void MasterWidget::mousePressEvent(QMouseEvent* event) {
    stopCameraAnimation();
    m_lastMousePos = event->pos();
    setFocus();

    Qt::MouseButtons btns = event->buttons();
    Qt::KeyboardModifiers mods = event->modifiers();

    if (m_navPreset == NavigationPreset::UG_NX) {
        // 西门子 UG (Siemens NX) 交互手感优化：
        // 1. 平移: 左键按住拖动 (同时兼容中+右、Shift+中键)
        if ((btns & Qt::LeftButton) ||
            ((btns & Qt::MiddleButton) && (btns & Qt::RightButton)) ||
            ((btns & Qt::MiddleButton) && (mods & Qt::ShiftModifier))) {
            m_isPanning = true;
            m_isRotating = false;
            m_isZooming = false;
            setCursor(Qt::SizeAllCursor);
        }
        // 2. 动态缩放: Ctrl + 中键 拖动
        else if ((btns & Qt::MiddleButton) && (mods & Qt::ControlModifier)) {
            m_isZooming = true;
            m_isRotating = false;
            m_isPanning = false;
            setCursor(Qt::SizeVerCursor);
        }
        // 3. 旋转: 右键 或 中键 按下拖动
        else if ((btns & Qt::RightButton) || (btns & Qt::MiddleButton)) {
            m_isRotating = true;
            m_isPanning = false;
            m_isZooming = false;
            setCursor(Qt::CrossCursor);
        }
    } else if (m_navPreset == NavigationPreset::SolidWorks) {
        // SolidWorks: 中键旋转, Ctrl+中键平移, Shift+中键缩放
        if ((btns & Qt::MiddleButton) && (mods & Qt::ControlModifier)) {
            m_isPanning = true;
            setCursor(Qt::SizeAllCursor);
        } else if ((btns & Qt::MiddleButton) && (mods & Qt::ShiftModifier)) {
            m_isZooming = true;
            setCursor(Qt::SizeVerCursor);
        } else if (btns & Qt::MiddleButton) {
            m_isRotating = true;
            setCursor(Qt::CrossCursor);
        }
    } else {
        // DefaultGeneric (经典通用): 左键旋转, 右键/中键平移
        if (btns & Qt::LeftButton) {
            m_isRotating = true;
            setCursor(Qt::CrossCursor);
        } else if ((btns & Qt::RightButton) || (btns & Qt::MiddleButton)) {
            m_isPanning = true;
            setCursor(Qt::SizeAllCursor);
        }
    }
}

void MasterWidget::mouseMoveEvent(QMouseEvent* event) {
    int dx = event->x() - m_lastMousePos.x();
    int dy = event->y() - m_lastMousePos.y();
    m_lastMousePos = event->pos();

    Qt::MouseButtons btns = event->buttons();
    Qt::KeyboardModifiers mods = event->modifiers();

    // 在 UG NX 模式下，允许在拖动中途动态切换模式
    if (m_navPreset == NavigationPreset::UG_NX) {
        if ((btns & Qt::LeftButton) ||
            ((btns & Qt::MiddleButton) && (btns & Qt::RightButton)) ||
            ((btns & Qt::MiddleButton) && (mods & Qt::ShiftModifier))) {
            m_isPanning = true;
            m_isRotating = false;
            m_isZooming = false;
            setCursor(Qt::SizeAllCursor);
        } else if ((btns & Qt::MiddleButton) && (mods & Qt::ControlModifier)) {
            m_isZooming = true;
            m_isRotating = false;
            m_isPanning = false;
            setCursor(Qt::SizeVerCursor);
        } else if ((btns & Qt::RightButton) || (btns & Qt::MiddleButton)) {
            m_isRotating = true;
            m_isPanning = false;
            m_isZooming = false;
            setCursor(Qt::CrossCursor);
        }
    }

    if (m_isRotating) {
        float rotX = static_cast<float>(dy) * 0.4f;
        float rotY = static_cast<float>(dx) * 0.4f;

        QQuaternion qX = QQuaternion::fromAxisAndAngle(QVector3D(1, 0, 0), rotX);
        QQuaternion qY = QQuaternion::fromAxisAndAngle(QVector3D(0, 1, 0), rotY);
        m_rotation = qX * m_rotation * qY;
        update();
    } else if (m_isPanning) {
        float factor = m_cameraDistance * 0.0015f;
        QVector3D pan(-dx * factor, dy * factor, 0.0f);
        m_cameraTarget += m_rotation.rotatedVector(pan);
        update();
    } else if (m_isZooming) {
        // 动态光标缩放：垂直拖动，向上为放大，向下为缩小
        float zoomFactor = 1.0f + static_cast<float>(dy) * 0.006f;
        zoomFactor = std::clamp(zoomFactor, 0.5f, 2.0f);
        m_cameraDistance = std::clamp(m_cameraDistance * zoomFactor, minCameraDistance(), maxCameraDistance());
        updateProjectionMatrix();
        update();
    }
}

void MasterWidget::mouseReleaseEvent(QMouseEvent* event) {
    Q_UNUSED(event);
    Qt::MouseButtons btns = event->buttons();

    if (m_navPreset == NavigationPreset::UG_NX) {
        if (!(btns & (Qt::LeftButton | Qt::MiddleButton | Qt::RightButton))) {
            m_isRotating = false;
            m_isPanning = false;
            m_isZooming = false;
            unsetCursor();
        } else if (btns & (Qt::MiddleButton | Qt::RightButton)) {
            // 中键或右键仍按住，若左键松开了则无缝切回旋转
            if (!(btns & Qt::LeftButton) && !(event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
                m_isPanning = false;
                m_isZooming = false;
                m_isRotating = true;
                setCursor(Qt::CrossCursor);
            }
        } else if (btns & Qt::LeftButton) {
            m_isRotating = false;
            m_isZooming = false;
            m_isPanning = true;
            setCursor(Qt::SizeAllCursor);
        }
    } else {
        if (!(btns & Qt::LeftButton)) m_isRotating = false;
        if (!(btns & (Qt::RightButton | Qt::MiddleButton))) m_isPanning = false;
        m_isZooming = false;
        if (btns == Qt::NoButton) {
            unsetCursor();
        }
    }
}

void MasterWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        // UG NX 双击视口自适应居中充满全屏
        fitView();
    }
    QOpenGLWidget::mouseDoubleClickEvent(event);
}

void MasterWidget::wheelEvent(QWheelEvent* event) {
    QPoint numPixels = event->pixelDelta();
    QPoint numDegrees = event->angleDelta() / 8;

    int dy = 0;
    if (!numPixels.isNull()) {
        dy = numPixels.y();
    } else if (!numDegrees.isNull()) {
        dy = numDegrees.y();
    }
    if (dy == 0) return;

    // UG NX 滚轮手感：向前滚动(远离用户, dy > 0)为放大，向后滚动(靠近用户, dy < 0)为缩小
    float factor = (dy > 0) ? 0.85f : 1.18f;

    // 获取当前鼠标指针在 3D 焦点平面上的世界坐标 (Zoom-to-Cursor 锚点)
    QPoint mousePos = event->position().toPoint();
    QVector3D pWorld = getCursorWorldPointOnFocusPlane(mousePos);

    float oldDist = m_cameraDistance;
    float newDist = std::clamp(oldDist * factor, minCameraDistance(), maxCameraDistance());

    // 锚定光标点屏幕位置不发生偏移：T_new = T_old + (P_world - T_old) * (1 - newDist / oldDist)
    float ratio = newDist / oldDist;
    m_cameraTarget += (pWorld - m_cameraTarget) * (1.0f - ratio);
    m_cameraDistance = newDist;

    updateProjectionMatrix();
    update();
}

void MasterWidget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        emit sigCancelRequested();
        setLoading(false);
    } else if (event->matches(QKeySequence::Find) ||
               ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_F)) {
        // UG NX 标准快捷键 Ctrl + F：适合屏幕自适应
        fitView();
    } else if (event->key() == Qt::Key_R || event->key() == Qt::Key_Home) {
        switchCamera(CameraPreset::Reset);
    }
    QOpenGLWidget::keyPressEvent(event);
}
