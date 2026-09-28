#pragma once

#include "model_data.h"
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QMatrix4x4>
#include <QQuaternion>
#include <QVector3D>
#include <QTimer>
#include <QElapsedTimer>
#include <memory>

/**
 * @brief 紧凑型顶点布局结构体 (24字节: 12B pos + 12B normal)，节约 40% VRAM 显存带宽
 */
struct CompactVertex {
    QVector3D position;
    QVector3D normal;
    CompactVertex() = default;
    CompactVertex(const QVector3D& p, const QVector3D& n) : position(p), normal(n) {}
};

/**
 * @brief 3D 视口画布 (QOpenGLWidget)，具备全彩材质渲染与平滑多线程加载遮罩
 */
class MasterWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    enum class ShadingMode {
        MaterialColor, // 材质固有色彩 / 纹理
        VertexColor,   // 顶点属性色
        PureWhite,     // 工业纯白结构模
        Normals        // 法线朝向着色
    };

    enum class CameraPreset {
        Front, Back, Left, Right, Top, Bottom, Reset
    };

    enum class NavigationPreset {
        UG_NX,          // 西门子 UG (Siemens NX): 中键旋转, 中+右/Shift+中平移, Ctrl+中/滚轮聚焦缩放
        SolidWorks,     // SolidWorks: 中键旋转, Ctrl+中键平移, 滚轮缩放
        DefaultGeneric  // 经典通用: 左键旋转, 右键平移, 滚轮缩放
    };

    explicit MasterWidget(QWidget* parent = nullptr);
    virtual ~MasterWidget() override;

    void setModel(ModelDataPtr model);
    void setLoading(bool loading, const QString& status = QString());
    void setErrorMessage(const QString& msg);
    void setShadingMode(ShadingMode mode);
    void setNavigationPreset(NavigationPreset preset);
    NavigationPreset navigationPreset() const { return m_navPreset; }
    void setShowGrid(bool show);
    void setShowAxis(bool show);
    void setShowWireframe(bool show);
    void setShowFeatureEdges(bool show);
    void setShowBoundingBox(bool show);
    bool isShowBoundingBox() const { return m_showBoundingBox; }
    void setOrthographic(bool ortho);

    // 动态轴向剖切面 (Section Peek)
    void setSectionEnabled(bool enabled);
    void setSectionAxis(int axis); // 0: X, 1: Y, 2: Z
    void setSectionDepth(float depthPercent); // 0.0f ~ 1.0f
    bool isSectionEnabled() const { return m_sectionEnabled; }
    int sectionAxis() const { return m_sectionAxis; }
    float sectionDepth() const { return m_sectionDepthPercent; }

    void switchCamera(CameraPreset preset);
    void fitView(); // 自适应居中充满全屏 (Ctrl+F 或双击空白)

    // 零件独立显隐控制
    void setPartVisible(int partId, bool visible);
    void setAllPartsVisible(bool visible);
    bool isPartVisible(int partId) const;

    // 相机球面插值平滑运镜 (Camera Slerp Smoothing)
    void animateCameraTo(const QQuaternion& targetRot, const QVector3D& targetTarget, float targetDist, int durationMs = 200);
    void stopCameraAnimation();

signals:
    void sigCancelRequested();

protected:
    virtual void initializeGL() override;
    virtual void paintGL() override;
    virtual void paintEvent(QPaintEvent* event) override;
    virtual void resizeGL(int w, int h) override;

    virtual void mousePressEvent(QMouseEvent* event) override;
    virtual void mouseMoveEvent(QMouseEvent* event) override;
    virtual void mouseReleaseEvent(QMouseEvent* event) override;
    virtual void mouseDoubleClickEvent(QMouseEvent* event) override;
    virtual void wheelEvent(QWheelEvent* event) override;
    virtual void keyPressEvent(QKeyEvent* event) override;

private:
    void initShaders();
    void buildBuffers();
    void updateProjectionMatrix();
    void drawGrid();
    void drawAxis();
    void drawBoundingBox();
    void drawBoundingBoxDimensions(QPainter& painter);
    void drawLoadingSpinner(QPainter& painter);
    void drawErrorMessage(QPainter& painter);
    float minCameraDistance() const;
    float maxCameraDistance() const;

private:
    ModelDataPtr m_model;

    // OpenGL 渲染资源
    std::unique_ptr<QOpenGLShaderProgram> m_program;
    std::unique_ptr<QOpenGLShaderProgram> m_lineProgram;

    struct GLMesh {
        std::shared_ptr<QOpenGLVertexArrayObject> vao;
        std::shared_ptr<QOpenGLBuffer> vbo;
        std::shared_ptr<QOpenGLBuffer> ibo;
        int indexCount = 0;
        QVector3D diffuseColor;
        bool hasVertexColor = false;
        bool isCompact = false; // 是否采用 24B 紧凑型顶点布局
        uint32_t primitiveType = 0x0004; // GL_TRIANGLES = 0x0004, GL_LINES = 0x0001
        float lineWidth = 1.5f;
        bool isFeatureEdge = false;
        bool visible = true;    // 零件显隐状态
        int partId = 0;         // 归属的零件装配节点 ID
        QMatrix4x4 transform;   // 实例世界空间变换矩阵
        int prototypeId = -1;   // 引用的原型 ID
        QVector3D worldAabbMin; // 世界空间包围盒下界 (用于视锥体剔除 Frustum Culling)
        QVector3D worldAabbMax; // 世界空间包围盒上界
        bool hasWorldAabb = false;
    };
    QVector<GLMesh> m_glMeshes;

    // 地面标尺网格静态 GPU 缓冲
    QOpenGLBuffer m_gridVbo{QOpenGLBuffer::VertexBuffer};
    int m_gridVertexCount = 0;
    void updateGridBuffer();

    // 相机与投影
    QMatrix4x4 m_projMatrix;
    QMatrix4x4 m_viewMatrix;
    QVector3D m_cameraTarget;
    float m_cameraDistance = 5.0f;
    QQuaternion m_rotation;

    // 相机球面插值平滑运镜 (Camera Slerp Smoothing)
    QTimer* m_cameraAnimTimer = nullptr;
    QElapsedTimer m_cameraAnimElapsed;
    int m_cameraAnimDuration = 200;
    QQuaternion m_animStartRot;
    QQuaternion m_animTargetRot;
    QVector3D m_animStartTarget;
    QVector3D m_animTargetTarget;
    float m_animStartDist = 5.0f;
    float m_animTargetDist = 5.0f;
    bool m_isCameraAnimating = false;

    // 交互状态
    QPoint m_lastMousePos;
    bool m_isRotating = false;
    bool m_isPanning = false;
    bool m_isZooming = false;
    NavigationPreset m_navPreset = NavigationPreset::UG_NX; // 默认西门子 UG 交互手感
    QVector3D getCursorWorldPointOnFocusPlane(const QPoint& screenPos);

    // 显示特性开关
    ShadingMode m_shadingMode = ShadingMode::MaterialColor;
    bool m_showGrid = true;
    bool m_showAxis = true;
    bool m_showWireframe = false;
    bool m_showFeatureEdges = true;
    bool m_showBoundingBox = false;
    bool m_orthographic = false;

    // 动态轴向剖切配置
    bool m_sectionEnabled = false;
    int m_sectionAxis = 1; // 0: X, 1: Y, 2: Z (默认垂直 Y 轴剖切)
    float m_sectionDepthPercent = 0.5f; // 0.0f ~ 1.0f 剖切深度百分比

    // 加载动画与状态
    bool m_isLoading = false;
    bool m_shadersValid = false;
    QString m_loadingStatus;
    QString m_errorMessage;
    float m_spinnerAngle = 0.0f;
    QTimer* m_animTimer = nullptr;
};
