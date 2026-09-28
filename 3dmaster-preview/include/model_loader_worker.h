#pragma once

#include "model_data.h"
#include <QThread>
#include <QObject>
#include <atomic>

#include <Standard_Handle.hxx>

class TopoDS_Shape;
class XCAFDoc_ColorTool;

/**
 * @brief 后台异步多线程 3D 模型加载解析器 (彻底解决大文件卡死 UI 线程)
 */
class ModelLoaderWorker : public QObject {
    Q_OBJECT
public:
    explicit ModelLoaderWorker(const QString& filePath, QObject* parent = nullptr);
    virtual ~ModelLoaderWorker() override;

    void requestCancel();
    bool isCancelled() const { return m_cancelRequested.load(); }

public slots:
    void startLoading();

signals:
    void sigStarted();
    void sigProgress(int percent, const QString& status);
    void sigFinished(ModelDataPtr model);
    void sigFailed(const QString& errorMessage);

private:
    ModelDataPtr parseModel(const QString& path);
    bool parseSTL(const QString& path, ModelDataPtr outModel);
    bool parseOBJ(const QString& path, ModelDataPtr outModel);
    bool parsePLY(const QString& path, ModelDataPtr outModel);
    bool parseSTEP(const QString& path, ModelDataPtr outModel);
    bool parseIGES(const QString& path, ModelDataPtr outModel);
    bool parseGLTF(const QString& path, ModelDataPtr outModel);
    bool parse3MF(const QString& path, ModelDataPtr outModel);
    bool parseOFF(const QString& path, ModelDataPtr outModel);
    bool parsePRT(const QString& path, ModelDataPtr outModel);

    // 通用 CAD 拓扑几何离散化与特征硬轮廓线提取 (STEP, IGES & GLTF 共享内核)
    bool processShapeTessellation(const TopoDS_Shape& shape,
                                  const QString& baseName,
                                  ModelDataPtr outModel,
                                  const occ::handle<XCAFDoc_ColorTool>& colorTool = occ::handle<XCAFDoc_ColorTool>(),
                                  int partId = 0,
                                  const std::optional<QVector3D>& customPartColor = std::nullopt,
                                  int prototypeId = -1,
                                  const QMatrix4x4& instanceTransform = QMatrix4x4());

    // 原型单次剖分生成网格组 (供装配体实例化零开销复用)
    QVector<SubMesh> tessellatePrototype(const TopoDS_Shape& shape,
                                         const QString& baseName,
                                         const occ::handle<XCAFDoc_ColorTool>& colorTool,
                                         const std::optional<QVector3D>& customPartColor,
                                         int prototypeId,
                                         double linearDeflectionScale = 1.0);

    // 简体中文路径与特殊字符防御解析 (Windows 8.3 短文件名与安全别名)
    QString resolveSafePath(const QString& inputPath);

private:
    QString m_filePath;
    QString m_customErrorMessage;
    std::atomic<bool> m_cancelRequested{false};
};
