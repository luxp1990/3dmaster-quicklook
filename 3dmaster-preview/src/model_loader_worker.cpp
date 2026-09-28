#include "model_loader_worker.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>
#include <QDataStream>
#include <QCryptographicHash>
#include <QDebug>
#include <QDateTime>
#include <QStandardPaths>
#include <QProcess>
#include <QSettings>
#include <QRandomGenerator>
#include <QCoreApplication>
#include <cmath>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <optional>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <fstream>
#include <filesystem>

// OpenCASCADE Technology 8.0 B-Rep, IGES, Mesh & ShapeFix Headers
#include <STEPControl_Reader.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <TDocStd_Document.hxx>
#include <Quantity_Color.hxx>
#include <Quantity_ColorRGBA.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDF_Tool.hxx>
#include <TDataStd_Name.hxx>
#include <IGESControl_Reader.hxx>
#include <ShapeFix_Shape.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <Poly_Triangulation.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <Poly_Polygon3D.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepLProp_SLProps.hxx>
#include <Geom2d_Curve.hxx>
#include <GeomAbs_Shape.hxx>
#include <Standard_Failure.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepLib.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressRange.hxx>

// OpenCASCADE 原生合作式毫秒级取消指示器 (Cooperative UserBreak Indicator)
class OcctCancelIndicator : public Message_ProgressIndicator {
public:
    explicit OcctCancelIndicator(std::atomic<bool>& cancelFlag) : m_cancelFlag(cancelFlag) {}
protected:
    bool UserBreak() override {
        return m_cancelFlag.load();
    }
    void Show(const Message_ProgressScope&, const bool) override {}
private:
    std::atomic<bool>& m_cancelFlag;
};

// OpenCASCADE Technology glTF Reader
#include <DEGLTF_Provider.hxx>
#include <DEGLTF_ConfigurationNode.hxx>

// Qt 3MF Reader (ZIP + XML)
#include <QtCore/private/qzipreader_p.h>
#include <QXmlStreamReader>
#include <QColor>
#include <QMatrix4x4>

static void traceWorkerLog(const QString& msg) {
    qDebug() << "[3dmaster Worker]" << msg;
    const QString logPath = QDir(QDir::tempPath()).filePath("3dmaster.log");
    QFile f(logPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&f);
        out << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz") << " [Worker] " << msg << "\n";
    }
}

// OpenCASCADE 全局进程级会话互斥锁 (杜绝多线程并发调用 XCAFApp 单例引发内存竞态)
static std::mutex g_occtSessionMutex;

// OpenCASCADE XCAF 文档 RAII 安全守卫 (彻底解决异常与提前退出导致的句柄泄漏)
struct XcafDocGuard {
    occ::handle<TDocStd_Document> doc;
    ~XcafDocGuard() {
        if (!doc.IsNull()) {
            try {
                XCAFApp_Application::GetApplication()->Close(doc);
            } catch (...) {}
            doc.Nullify();
        }
    }
};

// OpenCASCADE TopLoc_Location 转换至 Qt QMatrix4x4 实例世界变换矩阵
static QMatrix4x4 occLocToQMatrix(const TopLoc_Location& loc) {
    if (loc.IsIdentity()) return QMatrix4x4();
    const gp_Trsf& trsf = loc.Transformation();
    return QMatrix4x4(
        static_cast<float>(trsf.Value(1, 1)), static_cast<float>(trsf.Value(1, 2)), static_cast<float>(trsf.Value(1, 3)), static_cast<float>(trsf.Value(1, 4)),
        static_cast<float>(trsf.Value(2, 1)), static_cast<float>(trsf.Value(2, 2)), static_cast<float>(trsf.Value(2, 3)), static_cast<float>(trsf.Value(2, 4)),
        static_cast<float>(trsf.Value(3, 1)), static_cast<float>(trsf.Value(3, 2)), static_cast<float>(trsf.Value(3, 3)), static_cast<float>(trsf.Value(3, 4)),
        0.0f, 0.0f, 0.0f, 1.0f
    );
}

// 提取 OpenCASCADE TDF_Label 原生 UTF-16 中文零件名 (杜绝 AsciiString 丢失中文字符导致全是问号)
static QString extractOccName(const TDF_Label& label) {
    occ::handle<TDataStd_Name> nameAttr;
    if (label.FindAttribute(TDataStd_Name::GetID(), nameAttr)) {
        const TCollection_ExtendedString& extStr = nameAttr->Get();
        if (!extStr.IsEmpty()) {
            return QString::fromUtf16(reinterpret_cast<const char16_t*>(extStr.ToExtString()));
        }
    }
    return QString();
}

// 判定 CAD 提取的颜色是否为有效色彩 (过滤 (0,0,0) 近纯黑未初始化脏颜色)
static bool isValidCadColor(float r, float g, float b) {
    return (r * r + g * g + b * b) > 0.005f;
}
static bool isValidCadColor(const QVector3D& c) {
    return isValidCadColor(c.x(), c.y(), c.z());
}

// 进程内三维模型几何剖分 LRU 内存缓存 (Tessellation LRU Memory Cache)
// 避免二次打开同一 STP/3MF/CAD 模型耗费数秒重新 IncrementalMesh，实现 0ms 瞬间秒开
struct ModelCacheKey {
    QString path;
    qint64 fileSize = 0;
    qint64 lastModifiedMs = 0;

    bool operator==(const ModelCacheKey& o) const {
        return path == o.path && fileSize == o.fileSize && lastModifiedMs == o.lastModifiedMs;
    }
};

static std::mutex g_modelCacheMutex;
static std::list<std::pair<ModelCacheKey, ModelDataPtr>> g_modelCacheList;
static constexpr size_t MAX_CACHED_MODELS = 8; // 保留最近 8 个打开的模型

static ModelDataPtr getModelFromCache(const QString& path) {
    QFileInfo fi(path);
    if (!fi.exists()) return nullptr;
    ModelCacheKey key{ fi.canonicalFilePath(), fi.size(), fi.lastModified().toMSecsSinceEpoch() };

    std::lock_guard<std::mutex> lock(g_modelCacheMutex);
    for (auto it = g_modelCacheList.begin(); it != g_modelCacheList.end(); ++it) {
        if (it->first == key) {
            // LRU 命中，移至队首
            ModelDataPtr cached = it->second;
            if (it != g_modelCacheList.begin()) {
                g_modelCacheList.splice(g_modelCacheList.begin(), g_modelCacheList, it);
            }
            traceWorkerLog("getModelFromCache: HIT for " + key.path);
            return cached;
        }
    }
    return nullptr;
}

static void storeModelToCache(const QString& path, const ModelDataPtr& model) {
    if (!model || model->meshes.isEmpty()) return;
    QFileInfo fi(path);
    if (!fi.exists()) return;
    ModelCacheKey key{ fi.canonicalFilePath(), fi.size(), fi.lastModified().toMSecsSinceEpoch() };

    std::lock_guard<std::mutex> lock(g_modelCacheMutex);
    // 移除已有旧记录
    for (auto it = g_modelCacheList.begin(); it != g_modelCacheList.end(); ++it) {
        if (it->first.path == key.path) {
            g_modelCacheList.erase(it);
            break;
        }
    }
    // 插入队首
    g_modelCacheList.push_front({ key, model });
    while (g_modelCacheList.size() > MAX_CACHED_MODELS) {
        g_modelCacheList.pop_back();
    }
    traceWorkerLog("storeModelToCache: STORED for " + key.path + " (Cache size: " + QString::number(g_modelCacheList.size()) + ")");
}

ModelLoaderWorker::ModelLoaderWorker(const QString& filePath, QObject* parent)
    : QObject(parent), m_filePath(filePath) {}

ModelLoaderWorker::~ModelLoaderWorker() {
    requestCancel();
}

void ModelLoaderWorker::requestCancel() {
    m_cancelRequested.store(true);
}

QString ModelLoaderWorker::resolveSafePath(const QString& inputPath) {
    // 检查路径中是否包含非 ASCII 字符（如简体中文文件夹或文件名）
    bool hasNonAscii = false;
    for (const QChar& ch : inputPath) {
        if (ch.unicode() > 127) {
            hasNonAscii = true;
            break;
        }
    }

    if (!hasNonAscii) return inputPath;

#ifdef Q_OS_WIN
    // 方案一：优先获取 Windows 8.3 短文件名 (例如 D:\CAD~1\PART~1.STP，100% 纯 ASCII，零 I/O 开销)
    std::wstring wpath = inputPath.toStdWString();
    DWORD len = GetShortPathNameW(wpath.c_str(), nullptr, 0);
    if (len > 0) {
        std::vector<wchar_t> buffer(len);
        if (GetShortPathNameW(wpath.c_str(), buffer.data(), len) > 0) {
            QString shortPath = QString::fromWCharArray(buffer.data());
            bool shortAscii = true;
            for (const QChar& c : shortPath) {
                if (c.unicode() > 127) {
                    shortAscii = false;
                    break;
                }
            }
            if (shortAscii && QFile::exists(shortPath)) {
                return shortPath;
            }
        }
    }
#endif

    // 方案二：若当前磁盘卷未开启 8.3 短文件名，建立安全别名映射（避免第三方解析库不支持中文）
    QString tempRoot = QDir::tempPath() + "/3dmaster_alias";
    QDir().mkpath(tempRoot);

    QByteArray hash = QCryptographicHash::hash(inputPath.toUtf8(), QCryptographicHash::Md5).toHex();
    QString ext = QFileInfo(inputPath).suffix();
    QString aliasPath = QString("%1/model_%2.%3").arg(tempRoot, QString::fromUtf8(hash), ext);

    if (!QFile::exists(aliasPath)) {
        QFile::copy(inputPath, aliasPath);
    }

    return aliasPath;
}

void ModelLoaderWorker::startLoading() {
    traceWorkerLog("startLoading: m_filePath = '" + m_filePath + "'");
    emit sigStarted();
    QString fileName = QFileInfo(m_filePath).fileName();
    emit sigProgress(5, QString("正在读取模型: %1...").arg(fileName));

    if (m_cancelRequested.load()) {
        traceWorkerLog("startLoading: Cancelled early");
        emit sigFailed("加载已取消");
        return;
    }

    // 优先使用原生 Unicode 路径直接解析（QFile 宽字符底层完美支持所有中文路径）
    traceWorkerLog("Calling parseModel with m_filePath: " + m_filePath);
    ModelDataPtr model = parseModel(m_filePath);
    if (!model && !m_cancelRequested.load()) {
        traceWorkerLog("parseModel with m_filePath returned null, trying resolveSafePath...");
        QString safePath = resolveSafePath(m_filePath);
        traceWorkerLog("resolveSafePath result: " + safePath);
        if (safePath != m_filePath && QFile::exists(safePath)) {
            model = parseModel(safePath);
        }
    }

    if (m_cancelRequested.load()) {
        traceWorkerLog("startLoading: Cancelled after parse");
        emit sigFailed("加载已取消");
        return;
    }

    if (!model || model->meshes.isEmpty()) {
        traceWorkerLog("startLoading: FAILED - model is null or meshes empty!");
        QString finalErr = m_customErrorMessage.isEmpty()
            ? QString("未能从 [%1] 解析出有效三维几何体，可能文件暂未包含网格面片或特征").arg(fileName)
            : m_customErrorMessage;
        emit sigFailed(finalErr);
        return;
    }

    traceWorkerLog(QString("startLoading: SUCCESS - parsed %1 meshes! Calculating bounds...").arg(model->meshes.size()));
    emit sigProgress(95, "正在计算模型三维包围盒与材质属性...");
    model->calculateBounds();

    if (m_cancelRequested.load()) {
        traceWorkerLog("startLoading: Cancelled before emitting sigFinished");
        emit sigFailed("加载已取消");
        return;
    }
    emit sigProgress(100, "加载完成");
    traceWorkerLog("startLoading: Emitting sigFinished(model)...");
    emit sigFinished(model);
    traceWorkerLog("startLoading: sigFinished(model) emitted!");
}

ModelDataPtr ModelLoaderWorker::parseModel(const QString& path) {
    traceWorkerLog("parseModel: path = '" + path + "'");

    // 优先从内存 LRU 缓存中检索已剖分网格 (0ms 瞬间秒开)
    ModelDataPtr cachedModel = getModelFromCache(path);
    if (cachedModel) {
        // 创建浅拷贝壳体 (QVector 隐式共享，几何数据零拷贝，独立保留本次路径与独立显隐状态)
        auto cloned = std::make_shared<ModelData>(*cachedModel);
        cloned->filePath = m_filePath;
        return cloned;
    }

    QFileInfo fi(path);
    QString ext = fi.suffix().toLower();

    auto model = std::make_shared<ModelData>();
    model->filePath = m_filePath; // 保持原始简体中文文件路径，供界面正确展示
    model->format = ext;

    bool success = false;
    if (ext == "stl") {
        success = parseSTL(path, model);
    } else if (ext == "obj") {
        success = parseOBJ(path, model);
    } else if (ext == "ply") {
        success = parsePLY(path, model);
    } else if (ext == "off") {
        success = parseOFF(path, model);
    } else if (ext == "stp" || ext == "step") {
        success = parseSTEP(path, model);
    } else if (ext == "igs" || ext == "iges") {
        success = parseIGES(path, model);
    } else if (ext == "glb" || ext == "gltf") {
        success = parseGLTF(path, model);
    } else if (ext == "3mf") {
        success = parse3MF(path, model);
    } else if (ext == "prt") {
        success = parsePRT(path, model);
    } else {
        traceWorkerLog("parseModel: Unsupported extension '" + ext + "'");
        success = false;
    }

    if (success && model && !model->meshes.isEmpty()) {
        model->calculateBounds(); // 预先计算包围盒与材质指标，作为不可变完整快照存入 LRU
        storeModelToCache(path, model);
        return model;
    }
    return nullptr;
}

bool ModelLoaderWorker::parseSTL(const QString& path, ModelDataPtr outModel) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;

    QByteArray header = file.read(80);
    if (header.size() < 80) return false;

    SubMesh mesh;
    mesh.name = QFileInfo(path).baseName();
    mesh.diffuseColor = QVector3D(0.40f, 0.70f, 0.95f); // 工业蓝色默认材质

    uint32_t triangleCount = 0;
    file.read(reinterpret_cast<char*>(&triangleCount), 4);

    // 空间哈希顶点焊接与曲面平滑结构体定义 (0.001mm = 1微米量化公差)
    struct PosKey {
        int64_t x, y, z;
        bool operator==(const PosKey& o) const {
            return x == o.x && y == o.y && z == o.z;
        }
    };
    struct PosKeyHash {
        size_t operator()(const PosKey& k) const {
            size_t h1 = std::hash<int64_t>{}(k.x);
            size_t h2 = std::hash<int64_t>{}(k.y);
            size_t h3 = std::hash<int64_t>{}(k.z);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };

    struct VertexCandidate {
        uint32_t vertexIndex;
        QVector3D refNormal;
        QVector4D color;
        bool hasColor;
    };

    std::unordered_map<PosKey, std::vector<VertexCandidate>, PosKeyHash> posMap;
    std::vector<QVector3D> accumulatedNormals;

    auto addVertex = [&](const QVector3D& pos, const QVector3D& normal, const QVector4D& col, bool hasColor) {
        PosKey key = { std::llround(pos.x() * 1000.0), std::llround(pos.y() * 1000.0), std::llround(pos.z() * 1000.0) };
        auto& cands = posMap[key];
        int matchedIdx = -1;
        for (const auto& cand : cands) {
            if (cand.hasColor == hasColor && (!hasColor || cand.color == col)) {
                // 若法线夹角 <= 45° (cos 45° ≈ 0.707f)，判定为同一光滑曲面并焊接法线
                if (QVector3D::dotProduct(cand.refNormal, normal) >= 0.707f) {
                    matchedIdx = static_cast<int>(cand.vertexIndex);
                    break;
                }
            }
        }
        if (matchedIdx >= 0) {
            accumulatedNormals[matchedIdx] += normal;
            mesh.indices.push_back(static_cast<uint32_t>(matchedIdx));
        } else {
            uint32_t newIdx = static_cast<uint32_t>(mesh.vertices.size());
            Vertex vert(pos, normal);
            if (hasColor) {
                vert.color = col;
                mesh.hasVertexColors = true;
            }
            mesh.vertices.push_back(vert);
            accumulatedNormals.push_back(normal);
            cands.push_back({ newIdx, normal, col, hasColor });
            mesh.indices.push_back(newIdx);
        }
    };

    qint64 expectedSize = 84 + static_cast<qint64>(triangleCount) * 50;
    if (file.size() == expectedSize) {
        // 二进制 STL
        mesh.vertices.reserve(triangleCount / 2);
        mesh.indices.reserve(triangleCount * 3);
        posMap.reserve(triangleCount / 2);
        accumulatedNormals.reserve(triangleCount / 2);

        const int chunkSize = 5000;
        QByteArray chunk;
        chunk.resize(chunkSize * 50);

        uint32_t readCount = 0;
        while (readCount < triangleCount) {
            if (m_cancelRequested.load()) return false;

            uint32_t currentChunk = std::min(static_cast<uint32_t>(chunkSize), triangleCount - readCount);
            qint64 bytesRead = file.read(chunk.data(), currentChunk * 50);
            if (bytesRead < currentChunk * 50) break;

            const char* ptr = chunk.constData();
            for (uint32_t i = 0; i < currentChunk; ++i) {
                float normX, normY, normZ;
                memcpy(&normX, ptr, 4);
                memcpy(&normY, ptr + 4, 4);
                memcpy(&normZ, ptr + 8, 4);
                QVector3D normal(normX, normY, normZ);

                float v0x, v0y, v0z, v1x, v1y, v1z, v2x, v2y, v2z;
                memcpy(&v0x, ptr + 12, 4); memcpy(&v0y, ptr + 16, 4); memcpy(&v0z, ptr + 20, 4);
                memcpy(&v1x, ptr + 24, 4); memcpy(&v1y, ptr + 28, 4); memcpy(&v1z, ptr + 32, 4);
                memcpy(&v2x, ptr + 36, 4); memcpy(&v2y, ptr + 40, 4); memcpy(&v2z, ptr + 44, 4);

                QVector3D p0(v0x, v0y, v0z);
                QVector3D p1(v1x, v1y, v1z);
                QVector3D p2(v2x, v2y, v2z);

                if (normal.isNull() || normal.lengthSquared() < 1e-6f) {
                    normal = QVector3D::crossProduct(p1 - p0, p2 - p0).normalized();
                    if (normal.isNull()) normal = QVector3D(0, 1, 0);
                }

                // 2 字节属性字节 (VisCAM/Magics STL 颜色扩展识别)
                uint16_t attr = 0;
                memcpy(&attr, ptr + 48, 2);
                bool hasColor = false;
                QVector4D col(1.0f, 1.0f, 1.0f, 1.0f);
                if (attr & 0x8000) {
                    float r = ((attr >> 10) & 0x1F) / 31.0f;
                    float g = ((attr >> 5) & 0x1F) / 31.0f;
                    float b = (attr & 0x1F) / 31.0f;
                    col = QVector4D(r, g, b, 1.0f);
                    hasColor = true;
                }

                addVertex(p0, normal, col, hasColor);
                addVertex(p1, normal, col, hasColor);
                addVertex(p2, normal, col, hasColor);

                ptr += 50;
            }

            readCount += currentChunk;
            int pct = 10 + static_cast<int>(80.0 * readCount / triangleCount);
            emit sigProgress(pct, QString("已解析 %1 / %2 面片...").arg(readCount).arg(triangleCount));
        }
    } else {
        // ASCII STL
        file.seek(0);
        QTextStream in(&file);
        QVector3D curNormal;
        QVector<QVector3D> triVerts;

        int linesRead = 0;
        while (!in.atEnd()) {
            if (++linesRead % 20000 == 0) {
                if (m_cancelRequested.load()) return false;
                emit sigProgress(30, "正在流式解析 ASCII STL...");
            }
            QString line = in.readLine().trimmed();
            if (line.startsWith("facet normal", Qt::CaseInsensitive)) {
                auto parts = line.split(' ', Qt::SkipEmptyParts);
                if (parts.size() >= 5) {
                    curNormal = QVector3D(parts[2].toFloat(), parts[3].toFloat(), parts[4].toFloat());
                }
                triVerts.clear();
            } else if (line.startsWith("vertex", Qt::CaseInsensitive)) {
                auto parts = line.split(' ', Qt::SkipEmptyParts);
                if (parts.size() >= 4) {
                    triVerts.push_back(QVector3D(parts[1].toFloat(), parts[2].toFloat(), parts[3].toFloat()));
                }
            } else if (line.startsWith("endfacet", Qt::CaseInsensitive)) {
                if (triVerts.size() == 3) {
                    if (curNormal.isNull() || curNormal.lengthSquared() < 1e-6f) {
                        curNormal = QVector3D::crossProduct(triVerts[1] - triVerts[0], triVerts[2] - triVerts[0]).normalized();
                        if (curNormal.isNull()) curNormal = QVector3D(0, 1, 0);
                    }
                    addVertex(triVerts[0], curNormal, QVector4D(1,1,1,1), false);
                    addVertex(triVerts[1], curNormal, QVector4D(1,1,1,1), false);
                    addVertex(triVerts[2], curNormal, QVector4D(1,1,1,1), false);
                }
            }
        }
    }

    // 归一化所有焊接顶点的累积法向量，形成平滑且保锐角法线
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        QVector3D n = accumulatedNormals[i].normalized();
        if (!n.isNull()) {
            mesh.vertices[i].normal = n;
        }
    }

    outModel->meshes.push_back(mesh);
    return !mesh.vertices.isEmpty();
}

bool ModelLoaderWorker::parseOBJ(const QString& path, ModelDataPtr outModel) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;

    QFileInfo fi(path);
    QDir baseDir = fi.dir();

    QVector<QVector3D> positions;
    QVector<QVector3D> normals;
    QVector<QVector2D> texCoords;

    SubMesh currentMesh;
    currentMesh.name = fi.baseName();
    currentMesh.diffuseColor = QVector3D(0.85f, 0.72f, 0.55f); // 优雅木质/陶土暖色调

    QTextStream in(&file);
    int lineCounter = 0;

    while (!in.atEnd()) {
        if (++lineCounter % 30000 == 0) {
            if (m_cancelRequested.load()) return false;
            emit sigProgress(40, QString("已解析 %1 行 OBJ 几何数据...").arg(lineCounter));
        }

        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;

        QStringList tokens = line.split(' ', Qt::SkipEmptyParts);
        if (tokens.isEmpty()) continue;

        const QString& prefix = tokens[0];
        if (prefix == "v" && tokens.size() >= 4) {
            positions.push_back(QVector3D(tokens[1].toFloat(), tokens[2].toFloat(), tokens[3].toFloat()));
            // 如果带顶点色 (OBJ 扩展: v x y z r g b)
            if (tokens.size() >= 7) {
                float r = tokens[4].toFloat();
                float g = tokens[5].toFloat();
                float b = tokens[6].toFloat();
                currentMesh.hasVertexColors = true;
            }
        } else if (prefix == "vn" && tokens.size() >= 4) {
            normals.push_back(QVector3D(tokens[1].toFloat(), tokens[2].toFloat(), tokens[3].toFloat()));
        } else if (prefix == "vt" && tokens.size() >= 3) {
            texCoords.push_back(QVector2D(tokens[1].toFloat(), tokens[2].toFloat()));
        } else if (prefix == "usemtl" && tokens.size() >= 2) {
            // 遇到材质分件
            if (!currentMesh.vertices.isEmpty()) {
                outModel->meshes.push_back(currentMesh);
                currentMesh.vertices.clear();
                currentMesh.indices.clear();
            }
            currentMesh.name = tokens[1];
        } else if (prefix == "f" && tokens.size() >= 4) {
            QVector<uint32_t> faceIndices;
            for (int i = 1; i < tokens.size(); ++i) {
                QStringList elem = tokens[i].split('/');
                int pIdx = elem[0].toInt();
                int tIdx = elem.size() > 1 && !elem[1].isEmpty() ? elem[1].toInt() : 0;
                int nIdx = elem.size() > 2 && !elem[2].isEmpty() ? elem[2].toInt() : 0;

                QVector3D pos = (pIdx > 0 && pIdx <= positions.size()) ? positions[pIdx - 1] : QVector3D();
                QVector2D tex = (tIdx > 0 && tIdx <= texCoords.size()) ? texCoords[tIdx - 1] : QVector2D();
                QVector3D norm = (nIdx > 0 && nIdx <= normals.size()) ? normals[nIdx - 1] : QVector3D();

                uint32_t vIdx = static_cast<uint32_t>(currentMesh.vertices.size());
                currentMesh.vertices.push_back(Vertex(pos, norm, tex));
                faceIndices.push_back(vIdx);
            }

            // 多边形三角化 (Fan 三角剖分)
            for (int t = 1; t + 1 < faceIndices.size(); ++t) {
                currentMesh.indices.push_back(faceIndices[0]);
                currentMesh.indices.push_back(faceIndices[t]);
                currentMesh.indices.push_back(faceIndices[t + 1]);
            }
        }
    }

    if (!currentMesh.vertices.isEmpty()) {
        outModel->meshes.push_back(currentMesh);
    }

    return !outModel->meshes.isEmpty();
}

bool ModelLoaderWorker::parsePLY(const QString& path, ModelDataPtr outModel) {
    traceWorkerLog("parsePLY: Loading " + path);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;

    // 解析 PLY 头信息
    QByteArray line;
    bool isBinaryLittleEndian = false;
    int vertexCount = 0;
    int faceCount = 0;
    bool inVertexProps = false;
    int vertexByteSize = 0;
    bool hasColor = false;
    bool colorIsFloat = false;
    int xOffset = 0, yOffset = 4, zOffset = 8;
    int rOffset = -1, gOffset = -1, bOffset = -1;

    while (!(line = file.readLine().trimmed()).isEmpty()) {
        if (line == "end_header") break;
        QList<QByteArray> tokens = line.split(' ');
        if (tokens.isEmpty()) continue;

        if (tokens[0] == "format") {
            if (tokens.size() > 1 && tokens[1] == "binary_little_endian") {
                isBinaryLittleEndian = true;
            }
        } else if (tokens[0] == "element") {
            if (tokens.size() >= 3) {
                if (tokens[1] == "vertex") {
                    vertexCount = tokens[2].toInt();
                    inVertexProps = true;
                } else if (tokens[1] == "face") {
                    faceCount = tokens[2].toInt();
                    inVertexProps = false;
                } else {
                    inVertexProps = false;
                }
            }
        } else if (tokens[0] == "property") {
            if (inVertexProps && tokens.size() >= 3) {
                QString ptype = tokens[1];
                QString pname = tokens[2];
                int psize = 4;
                if (ptype == "char" || ptype == "uchar" || ptype == "uint8" || ptype == "int8") psize = 1;
                else if (ptype == "short" || ptype == "ushort" || ptype == "int16" || ptype == "uint16") psize = 2;
                else if (ptype == "int" || ptype == "uint" || ptype == "float" || ptype == "float32" || ptype == "int32" || ptype == "uint32") psize = 4;
                else if (ptype == "double" || ptype == "float64") psize = 8;

                if (pname == "x") xOffset = vertexByteSize;
                else if (pname == "y") yOffset = vertexByteSize;
                else if (pname == "z") zOffset = vertexByteSize;
                else if (pname == "red" || pname == "r") { rOffset = vertexByteSize; hasColor = true; colorIsFloat = (psize >= 4); }
                else if (pname == "green" || pname == "g") gOffset = vertexByteSize;
                else if (pname == "blue" || pname == "b") bOffset = vertexByteSize;

                vertexByteSize += psize;
            }
        }
    }

    if (vertexCount <= 0) return false;

    SubMesh mesh;
    mesh.name = QFileInfo(path).baseName();
    mesh.diffuseColor = QVector3D(0.45f, 0.75f, 0.65f); // 工业薄荷翠绿材质
    mesh.hasVertexColors = hasColor;
    mesh.vertices.reserve(vertexCount);

    if (isBinaryLittleEndian) {
        if (vertexByteSize <= 0) vertexByteSize = 12; // 默认 3 个 float
        QByteArray vBuffer = file.read(static_cast<qint64>(vertexCount) * vertexByteSize);
        const char* vPtr = vBuffer.constData();
        const qint64 bufSize = vBuffer.size();

        for (int i = 0; i < vertexCount; ++i) {
            if (i % 20000 == 0 && m_cancelRequested.load()) return false;
            qint64 offset = static_cast<qint64>(i) * vertexByteSize;
            if (offset + 12 > bufSize) break;

            float x = *reinterpret_cast<const float*>(vPtr + offset + xOffset);
            float y = *reinterpret_cast<const float*>(vPtr + offset + yOffset);
            float z = *reinterpret_cast<const float*>(vPtr + offset + zOffset);

            Vertex v(QVector3D(x, y, z));
            if (hasColor && rOffset >= 0 && gOffset >= 0 && bOffset >= 0) {
                if (colorIsFloat) {
                    float r = *reinterpret_cast<const float*>(vPtr + offset + rOffset);
                    float g = *reinterpret_cast<const float*>(vPtr + offset + gOffset);
                    float b = *reinterpret_cast<const float*>(vPtr + offset + bOffset);
                    v.color = QVector4D(r, g, b, 1.0f);
                } else {
                    uint8_t r = *reinterpret_cast<const uint8_t*>(vPtr + offset + rOffset);
                    uint8_t g = *reinterpret_cast<const uint8_t*>(vPtr + offset + gOffset);
                    uint8_t b = *reinterpret_cast<const uint8_t*>(vPtr + offset + bOffset);
                    v.color = QVector4D(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
                }
            }
            mesh.vertices.push_back(v);
        }

        // 读取 Face 拓扑面片
        if (faceCount > 0) {
            for (int f = 0; f < faceCount; ++f) {
                if (f % 20000 == 0 && m_cancelRequested.load()) return false;
                uint8_t nVerts = 0;
                if (file.read(reinterpret_cast<char*>(&nVerts), 1) != 1) break;
                if (nVerts < 3) continue;

                std::vector<uint32_t> fIndices(nVerts);
                for (int k = 0; k < nVerts; ++k) {
                    int32_t idx = 0;
                    file.read(reinterpret_cast<char*>(&idx), 4);
                    fIndices[k] = static_cast<uint32_t>(idx);
                }
                for (size_t t = 1; t + 1 < fIndices.size(); ++t) {
                    mesh.indices.push_back(fIndices[0]);
                    mesh.indices.push_back(fIndices[t]);
                    mesh.indices.push_back(fIndices[t + 1]);
                }
            }
        }
    } else {
        // ASCII PLY
        QTextStream in(&file);
        for (int i = 0; i < vertexCount; ++i) {
            if (i % 20000 == 0 && m_cancelRequested.load()) return false;
            while (!in.atEnd()) {
                QString vLine = in.readLine().trimmed();
                if (vLine.isEmpty()) continue;
                QTextStream ts(&vLine);
                float x = 0, y = 0, z = 0;
                ts >> x >> y >> z;
                Vertex v(QVector3D(x, y, z));
                if (hasColor) {
                    float r = 255, g = 255, b = 255;
                    ts >> r >> g >> b;
                    v.color = QVector4D(r > 1.0f ? r / 255.0f : r,
                                       g > 1.0f ? g / 255.0f : g,
                                       b > 1.0f ? b / 255.0f : b, 1.0f);
                }
                mesh.vertices.push_back(v);
                break;
            }
        }
        if (faceCount > 0) {
            for (int f = 0; f < faceCount; ++f) {
                if (f % 20000 == 0 && m_cancelRequested.load()) return false;
                while (!in.atEnd()) {
                    QString fLine = in.readLine().trimmed();
                    if (fLine.isEmpty()) continue;
                    QTextStream ts(&fLine);
                    int nVerts = 0;
                    ts >> nVerts;
                    if (nVerts >= 3) {
                        std::vector<uint32_t> fIndices(nVerts);
                        for (int k = 0; k < nVerts; ++k) ts >> fIndices[k];
                        for (size_t t = 1; t + 1 < fIndices.size(); ++t) {
                            mesh.indices.push_back(fIndices[0]);
                            mesh.indices.push_back(fIndices[t]);
                            mesh.indices.push_back(fIndices[t + 1]);
                        }
                    }
                    break;
                }
            }
        }
    }

    if (faceCount <= 0 || mesh.indices.isEmpty()) {
        // 点云模式 (Point Cloud)
        mesh.primitiveType = 0x0000; // GL_POINTS
        mesh.indices.resize(mesh.vertices.size());
        for (uint32_t i = 0; i < static_cast<uint32_t>(mesh.vertices.size()); ++i) {
            mesh.indices[i] = i;
        }
    } else {
        // 网格模式：计算平滑面法线与顶点法线
        mesh.primitiveType = 0x0004; // GL_TRIANGLES
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            uint32_t i1 = mesh.indices[i];
            uint32_t i2 = mesh.indices[i + 1];
            uint32_t i3 = mesh.indices[i + 2];
            if (i1 < mesh.vertices.size() && i2 < mesh.vertices.size() && i3 < mesh.vertices.size()) {
                QVector3D p1 = mesh.vertices[i1].position;
                QVector3D p2 = mesh.vertices[i2].position;
                QVector3D p3 = mesh.vertices[i3].position;
                QVector3D fn = QVector3D::crossProduct(p2 - p1, p3 - p1);
                if (fn.lengthSquared() > 1e-8f) {
                    mesh.vertices[i1].normal += fn;
                    mesh.vertices[i2].normal += fn;
                    mesh.vertices[i3].normal += fn;
                }
            }
        }
        for (auto& v : mesh.vertices) {
            if (v.normal.lengthSquared() > 1e-8f) {
                v.normal.normalize();
            } else {
                v.normal = QVector3D(0, 1, 0);
            }
        }
    }

    outModel->meshes.push_back(mesh);
    return !mesh.vertices.isEmpty();
}

bool ModelLoaderWorker::parseOFF(const QString& path, ModelDataPtr outModel) {
    traceWorkerLog("parseOFF: Loading " + path);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;

    QTextStream in(&file);
    QString header = in.readLine().trimmed();
    while (header.isEmpty() || header.startsWith("#")) {
        if (in.atEnd()) return false;
        header = in.readLine().trimmed();
    }

    int vertexCount = 0, faceCount = 0, edgeCount = 0;
    if (header.startsWith("OFF") && header.length() > 3) {
        QString rest = header.mid(3).trimmed();
        QTextStream ts(&rest);
        ts >> vertexCount >> faceCount >> edgeCount;
    } else if (header == "OFF") {
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith("#")) continue;
            QTextStream ts(&line);
            ts >> vertexCount >> faceCount >> edgeCount;
            break;
        }
    } else {
        return false;
    }

    if (vertexCount <= 0) return false;

    SubMesh mesh;
    mesh.name = QFileInfo(path).baseName();
    mesh.diffuseColor = QVector3D(0.85f, 0.55f, 0.25f); // 工业琥珀金材质
    mesh.primitiveType = 0x0004; // GL_TRIANGLES
    mesh.vertices.reserve(vertexCount);

    int readVerts = 0;
    while (!in.atEnd() && readVerts < vertexCount) {
        if (readVerts % 20000 == 0 && m_cancelRequested.load()) return false;
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith("#")) continue;
        QTextStream ts(&line);
        float x = 0, y = 0, z = 0;
        ts >> x >> y >> z;
        mesh.vertices.push_back(Vertex(QVector3D(x, y, z)));
        readVerts++;
    }

    int readFaces = 0;
    while (!in.atEnd() && readFaces < faceCount) {
        if (readFaces % 20000 == 0 && m_cancelRequested.load()) return false;
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith("#")) continue;
        QTextStream ts(&line);
        int nVerts = 0;
        ts >> nVerts;
        if (nVerts >= 3) {
            std::vector<uint32_t> fIndices(nVerts);
            for (int k = 0; k < nVerts; ++k) {
                ts >> fIndices[k];
            }
            for (size_t t = 1; t + 1 < fIndices.size(); ++t) {
                mesh.indices.push_back(fIndices[0]);
                mesh.indices.push_back(fIndices[t]);
                mesh.indices.push_back(fIndices[t + 1]);
            }
        }
        readFaces++;
    }

    // 计算法线
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        uint32_t i1 = mesh.indices[i];
        uint32_t i2 = mesh.indices[i + 1];
        uint32_t i3 = mesh.indices[i + 2];
        if (i1 < mesh.vertices.size() && i2 < mesh.vertices.size() && i3 < mesh.vertices.size()) {
            QVector3D p1 = mesh.vertices[i1].position;
            QVector3D p2 = mesh.vertices[i2].position;
            QVector3D p3 = mesh.vertices[i3].position;
            QVector3D fn = QVector3D::crossProduct(p2 - p1, p3 - p1);
            if (fn.lengthSquared() > 1e-8f) {
                mesh.vertices[i1].normal += fn;
                mesh.vertices[i2].normal += fn;
                mesh.vertices[i3].normal += fn;
            }
        }
    }
    for (auto& v : mesh.vertices) {
        if (v.normal.lengthSquared() > 1e-8f) {
            v.normal.normalize();
        } else {
            v.normal = QVector3D(0, 1, 0);
        }
    }

    if (!mesh.vertices.isEmpty()) {
        outModel->meshes.push_back(mesh);
        return true;
    }
    return false;
}

QVector<SubMesh> ModelLoaderWorker::tessellatePrototype(const TopoDS_Shape& shape,
                                                       const QString& baseName,
                                                       const occ::handle<XCAFDoc_ColorTool>& colorTool,
                                                       const std::optional<QVector3D>& customPartColor,
                                                       int prototypeId,
                                                       double linearDeflectionScale) {
    QVector<SubMesh> result;
    if (shape.IsNull()) {
        traceWorkerLog("tessellatePrototype: shape is null!");
        return result;
    }

    if (m_cancelRequested.load()) return result;

    Bnd_Box bndBox;
    BRepBndLib::AddClose(shape, bndBox);
    double xmin = 0, ymin = 0, zmin = 0, xmax = 0, ymax = 0, zmax = 0;
    if (!bndBox.IsVoid()) {
        bndBox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    }
    double dx = xmax - xmin;
    double dy = ymax - ymin;
    double dz = zmax - zmin;
    double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (diag < 1e-4) diag = 100.0;

    const double linearDeflection = std::clamp(diag * 0.00025 * linearDeflectionScale, 0.001, 5.0);
    const double angularDeflection = 0.20;

    traceWorkerLog(QString("tessellatePrototype: Proto %1 '%2' diag=%3 mm, deflection=%4 mm")
                   .arg(prototypeId).arg(baseName).arg(diag).arg(linearDeflection));

    occ::handle<OcctCancelIndicator> cancelIndicator = new OcctCancelIndicator(m_cancelRequested);
    BRepMesh_IncrementalMesh mesher;
    mesher.SetShape(shape);
    mesher.ChangeParameters().Deflection = linearDeflection;
    mesher.ChangeParameters().Angle = angularDeflection;
    mesher.ChangeParameters().InParallel = true;
    mesher.Perform(cancelIndicator->Start());

    if (m_cancelRequested.load()) return result;

    try {
        BRepLib::EnsureNormalConsistency(shape, 0.001, true);
    } catch (...) {}

    if (m_cancelRequested.load()) return result;

    SubMesh solidMesh;
    solidMesh.name = baseName + "_solid";
    solidMesh.diffuseColor = QVector3D(0.72f, 0.76f, 0.82f);
    solidMesh.primitiveType = 0x0004; // GL_TRIANGLES
    solidMesh.prototypeId = prototypeId;

    Quantity_ColorRGBA defaultShapeColor;
    bool hasDefaultShapeColor = false;
    if (customPartColor.has_value() && isValidCadColor(customPartColor.value())) {
        solidMesh.diffuseColor = customPartColor.value();
        defaultShapeColor = Quantity_ColorRGBA(
            Quantity_Color(solidMesh.diffuseColor.x(), solidMesh.diffuseColor.y(), solidMesh.diffuseColor.z(), Quantity_TOC_RGB), 1.0f);
        hasDefaultShapeColor = true;
    } else if (!colorTool.IsNull()) {
        if (colorTool->GetColor(shape, XCAFDoc_ColorGen, defaultShapeColor) ||
            colorTool->GetColor(shape, XCAFDoc_ColorSurf, defaultShapeColor)) {
            float r = static_cast<float>(defaultShapeColor.GetRGB().Red());
            float g = static_cast<float>(defaultShapeColor.GetRGB().Green());
            float b = static_cast<float>(defaultShapeColor.GetRGB().Blue());
            if (isValidCadColor(r, g, b)) {
                hasDefaultShapeColor = true;
                solidMesh.diffuseColor = QVector3D(r, g, b);
            }
        }
    }

    solidMesh.hasVertexColors = false;

    int faceColorOverrideCount = 0;
    QVector4D lastFaceColor;

    TopExp_Explorer faceExp(shape, TopAbs_FACE);
    for (; faceExp.More(); faceExp.Next()) {
        if (m_cancelRequested.load()) return result;

        const TopoDS_Face& face = TopoDS::Face(faceExp.Current());
        TopLoc_Location loc;
        occ::handle<Poly_Triangulation> tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull() || tri->NbNodes() == 0 || tri->NbTriangles() == 0) continue;

        QVector4D faceColor(solidMesh.diffuseColor.x(), solidMesh.diffuseColor.y(), solidMesh.diffuseColor.z(), 1.0f);
        if (!colorTool.IsNull()) {
            Quantity_ColorRGBA cRGBA;
            if (colorTool->GetColor(face, XCAFDoc_ColorSurf, cRGBA) ||
                colorTool->GetColor(face, XCAFDoc_ColorGen, cRGBA)) {
                float r = static_cast<float>(cRGBA.GetRGB().Red());
                float g = static_cast<float>(cRGBA.GetRGB().Green());
                float b = static_cast<float>(cRGBA.GetRGB().Blue());
                if (isValidCadColor(r, g, b)) {
                    faceColorOverrideCount++;
                    lastFaceColor = QVector4D(r, g, b, static_cast<float>(cRGBA.Alpha()));
                    faceColor = lastFaceColor;
                }
            }
        }

        gp_Trsf trsf = loc.Transformation();
        bool hasTrsf = !loc.IsIdentity();
        bool isReversed = (face.Orientation() == TopAbs_REVERSED);

        uint32_t vertexBase = static_cast<uint32_t>(solidMesh.vertices.size());
        int nbNodes = tri->NbNodes();

        for (int i = 1; i <= nbNodes; ++i) {
            gp_Pnt p = tri->Node(i);
            if (hasTrsf) {
                p.Transform(trsf);
            }

            gp_Dir nDir(0, 1, 0);
            try {
                if (tri->HasNormals()) {
                    nDir = tri->Normal(i);
                    if (hasTrsf) {
                        nDir.Transform(trsf);
                    }
                } else {
                    BRepAdaptor_Surface surf(face, Standard_False);
                    if (tri->HasUVNodes()) {
                        gp_Pnt2d uv = tri->UVNode(i);
                        BRepLProp_SLProps props(surf, uv.X(), uv.Y(), 1, 0.01);
                        if (props.IsNormalDefined()) {
                            nDir = props.Normal();
                            if (hasTrsf) {
                                nDir.Transform(trsf);
                            }
                        }
                    }
                }
            } catch (...) {
                nDir = gp_Dir(0, 1, 0);
            }

            if (isReversed) {
                nDir.Reverse();
            }

            Vertex v;
            v.position = QVector3D(static_cast<float>(p.X()), static_cast<float>(p.Y()), static_cast<float>(p.Z()));
            v.normal = QVector3D(static_cast<float>(nDir.X()), static_cast<float>(nDir.Y()), static_cast<float>(nDir.Z()));
            v.color = faceColor;

            if (tri->HasUVNodes()) {
                gp_Pnt2d uv = tri->UVNode(i);
                v.texCoord = QVector2D(static_cast<float>(uv.X()), static_cast<float>(uv.Y()));
            }

            solidMesh.vertices.push_back(v);
        }

        int nbTriangles = tri->NbTriangles();
        for (int i = 1; i <= nbTriangles; ++i) {
            Standard_Integer n1, n2, n3;
            tri->Triangle(i).Get(n1, n2, n3);
            if (isReversed) {
                std::swap(n2, n3);
            }
            solidMesh.indices.push_back(vertexBase + n1 - 1);
            solidMesh.indices.push_back(vertexBase + n2 - 1);
            solidMesh.indices.push_back(vertexBase + n3 - 1);
        }
    }

    // 仅当面级确实存在有效自定义色彩覆盖时，才开启顶点色模式 (防止默认颜色被固化到顶点色阻断实例级着色)
    if (faceColorOverrideCount > 0) {
        solidMesh.hasVertexColors = true;
    }

    if (!solidMesh.vertices.isEmpty()) {
        result.push_back(solidMesh);
    }

    // 特征硬轮廓线提取 (工业级容错)
    try {
        SubMesh edgeMesh;
        edgeMesh.name = baseName + "_edges";
        edgeMesh.diffuseColor = QVector3D(0.12f, 0.14f, 0.18f);
        edgeMesh.primitiveType = 0x0001; // GL_LINES
        edgeMesh.lineWidth = 1.5f;
        edgeMesh.isFeatureEdge = true;
        edgeMesh.prototypeId = prototypeId;

        TopTools_IndexedDataMapOfShapeListOfShape edgeToFaceMap;
        TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeToFaceMap);

    for (int i = 1; i <= edgeToFaceMap.Extent(); ++i) {
        if (m_cancelRequested.load()) return result;

        const TopoDS_Edge& edge = TopoDS::Edge(edgeToFaceMap.FindKey(i));
        const TopTools_ListOfShape& faces = edgeToFaceMap.FindFromIndex(i);

        if (BRep_Tool::Degenerated(edge)) continue;

        bool isFeatureEdge = false;
        if (faces.Extent() == 1) {
            isFeatureEdge = true;
        } else if (faces.Extent() == 2) {
            GeomAbs_Shape cont = BRep_Tool::Continuity(edge, TopoDS::Face(faces.First()), TopoDS::Face(faces.Last()));
            if (cont == GeomAbs_C0) {
                isFeatureEdge = true;
            }
        } else if (faces.Extent() > 2) {
            isFeatureEdge = true;
        }

        if (!isFeatureEdge) continue;

        occ::handle<Poly_PolygonOnTriangulation> polyOnTri;
        occ::handle<Poly_Triangulation> faceTri;
        TopLoc_Location locEdge;
        BRep_Tool::PolygonOnTriangulation(edge, polyOnTri, faceTri, locEdge);
        if (!polyOnTri.IsNull() && !faceTri.IsNull() && polyOnTri->NbNodes() >= 2) {
            gp_Trsf trsf = locEdge.Transformation();
            bool hasTrsf = !locEdge.IsIdentity();
            const TColStd_Array1OfInteger& nodeIndices = polyOnTri->Nodes();
            for (int j = nodeIndices.Lower(); j < nodeIndices.Upper(); ++j) {
                int n1 = nodeIndices.Value(j);
                int n2 = nodeIndices.Value(j + 1);
                if (n1 < 1 || n1 > faceTri->NbNodes() || n2 < 1 || n2 > faceTri->NbNodes()) continue;

                gp_Pnt p1 = faceTri->Node(n1);
                gp_Pnt p2 = faceTri->Node(n2);
                if (hasTrsf) {
                    p1.Transform(trsf);
                    p2.Transform(trsf);
                }

                uint32_t idx1 = static_cast<uint32_t>(edgeMesh.vertices.size());
                edgeMesh.vertices.push_back(Vertex(QVector3D(static_cast<float>(p1.X()), static_cast<float>(p1.Y()), static_cast<float>(p1.Z()))));
                edgeMesh.vertices.push_back(Vertex(QVector3D(static_cast<float>(p2.X()), static_cast<float>(p2.Y()), static_cast<float>(p2.Z()))));
                edgeMesh.indices.push_back(idx1);
                edgeMesh.indices.push_back(idx1 + 1);
            }
        } else {
            TopLoc_Location loc3d;
            const occ::handle<Poly_Polygon3D>& poly3d = BRep_Tool::Polygon3D(edge, loc3d);
            if (!poly3d.IsNull() && poly3d->NbNodes() >= 2) {
                gp_Trsf trsf = loc3d.Transformation();
                bool hasTrsf = !loc3d.IsIdentity();
                const NCollection_Array1<gp_Pnt>& nodes = poly3d->Nodes();
                for (int j = nodes.Lower(); j < nodes.Upper(); ++j) {
                    gp_Pnt p1 = nodes.Value(j);
                    gp_Pnt p2 = nodes.Value(j + 1);
                    if (hasTrsf) {
                        p1.Transform(trsf);
                        p2.Transform(trsf);
                    }
                    uint32_t idx1 = static_cast<uint32_t>(edgeMesh.vertices.size());
                    edgeMesh.vertices.push_back(Vertex(QVector3D(static_cast<float>(p1.X()), static_cast<float>(p1.Y()), static_cast<float>(p1.Z()))));
                    edgeMesh.vertices.push_back(Vertex(QVector3D(static_cast<float>(p2.X()), static_cast<float>(p2.Y()), static_cast<float>(p2.Z()))));
                    edgeMesh.indices.push_back(idx1);
                    edgeMesh.indices.push_back(idx1 + 1);
                }
            }
        }
    }

        if (!edgeMesh.vertices.isEmpty()) {
            result.push_back(edgeMesh);
        }
    } catch (...) {
        traceWorkerLog("tessellatePrototype: Feature edge extraction exception bypassed gracefully.");
    }

    return result;
}

bool ModelLoaderWorker::processShapeTessellation(const TopoDS_Shape& shape,
                                              const QString& baseName,
                                              ModelDataPtr outModel,
                                              const occ::handle<XCAFDoc_ColorTool>& colorTool,
                                              int partId,
                                              const std::optional<QVector3D>& customPartColor,
                                              int prototypeId,
                                              const QMatrix4x4& instanceTransform) {
    auto meshes = tessellatePrototype(shape, baseName, colorTool, customPartColor, prototypeId, 1.0);
    for (auto& m : meshes) {
        m.partId = partId;
        m.transform = instanceTransform;
        outModel->meshes.push_back(std::move(m));
    }
    return !meshes.isEmpty();
}

struct StepPrototype {
    int protoId = -1;
    TDF_Label protoLabel;
    TopoDS_Shape localShape;
    QString baseName;
    std::optional<QVector3D> defaultColor;
};

struct StepInstance {
    int partId = 0;
    QString name;
    int protoId = -1;
    TopLoc_Location location;
    std::optional<QVector3D> partColor;
};

static void walkAssemblyTree(const occ::handle<XCAFDoc_ShapeTool>& shapeTool,
                            const occ::handle<XCAFDoc_ColorTool>& colorTool,
                            const TDF_Label& L,
                            const TopLoc_Location& parentLoc,
                            std::vector<StepPrototype>& prototypes,
                            std::unordered_map<std::string, int>& entryToProtoId,
                            std::vector<StepInstance>& instances,
                            std::unordered_set<std::string>& recursionGuard,
                            int depth = 0) {
    if (depth > 64) {
        traceWorkerLog(QString("walkAssemblyTree: Depth limit (64) reached on label, skipping sub-branches."));
        return;
    }
    TopLoc_Location currentLoc = parentLoc * shapeTool->GetLocation(L);

    TDF_Label protoLabel = L;
    if (shapeTool->IsReference(L)) {
        shapeTool->GetReferredShape(L, protoLabel);
    }

    TCollection_AsciiString entryStr;
    TDF_Tool::Entry(protoLabel, entryStr);
    std::string entry = entryStr.ToCString();
    if (recursionGuard.count(entry)) {
        return; // 防循环引用
    }

    if (shapeTool->IsAssembly(protoLabel)) {
        recursionGuard.insert(entry);
        TDF_LabelSequence components;
        shapeTool->GetComponents(protoLabel, components, Standard_False);
        for (int i = 1; i <= components.Length(); ++i) {
            walkAssemblyTree(shapeTool, colorTool, components.Value(i), currentLoc,
                             prototypes, entryToProtoId, instances, recursionGuard, depth + 1);
        }
        recursionGuard.erase(entry);
        return;
    }

    // 叶子零件实体原型
    TopoDS_Shape protoShape = shapeTool->GetShape(protoLabel);
    if (protoShape.IsNull()) {
        protoShape = shapeTool->GetShape(L);
    }
    if (protoShape.IsNull()) return;

    // 收集原型 (若未登记)
    int protoId = -1;
    auto itProto = entryToProtoId.find(entry);
    if (itProto != entryToProtoId.end()) {
        protoId = itProto->second;
    } else {
        protoId = static_cast<int>(prototypes.size());
        entryToProtoId[entry] = protoId;

        // 原生 UTF-16 提取中文原型名
        QString protoName = extractOccName(protoLabel);
        if (protoName.trimmed().isEmpty()) {
            protoName = QString("原型_%1").arg(protoId + 1);
        }

        std::optional<QVector3D> protoColor;
        if (!colorTool.IsNull()) {
            Quantity_ColorRGBA cRGBA;
            if (colorTool->GetColor(protoLabel, XCAFDoc_ColorGen, cRGBA) ||
                colorTool->GetColor(protoLabel, XCAFDoc_ColorSurf, cRGBA) ||
                colorTool->GetColor(protoShape, XCAFDoc_ColorGen, cRGBA) ||
                colorTool->GetColor(protoShape, XCAFDoc_ColorSurf, cRGBA)) {
                float r = static_cast<float>(cRGBA.GetRGB().Red());
                float g = static_cast<float>(cRGBA.GetRGB().Green());
                float b = static_cast<float>(cRGBA.GetRGB().Blue());
                if (isValidCadColor(r, g, b)) {
                    protoColor = QVector3D(r, g, b);
                }
            } else {
                TopExp_Explorer fExp(protoShape, TopAbs_FACE);
                for (; fExp.More(); fExp.Next()) {
                    const TopoDS_Face& f = TopoDS::Face(fExp.Current());
                    if (colorTool->GetColor(f, XCAFDoc_ColorSurf, cRGBA) ||
                        colorTool->GetColor(f, XCAFDoc_ColorGen, cRGBA)) {
                        float r = static_cast<float>(cRGBA.GetRGB().Red());
                        float g = static_cast<float>(cRGBA.GetRGB().Green());
                        float b = static_cast<float>(cRGBA.GetRGB().Blue());
                        if (isValidCadColor(r, g, b)) {
                            protoColor = QVector3D(r, g, b);
                            break;
                        }
                    }
                }
            }
        }
        prototypes.push_back({ protoId, protoLabel, protoShape, protoName, protoColor });
    }

    // 提取实例名称 (原生 UTF-16，若实例名含乱码问号则智能回退至原型纯正中文名)
    QString instName = extractOccName(L);
    if (instName.trimmed().isEmpty() || (instName.contains('?') && !prototypes[protoId].baseName.contains('?'))) {
        instName = prototypes[protoId].baseName;
    }
    if (instName.trimmed().isEmpty()) {
        instName = QString("零件_%1").arg(instances.size() + 1);
    }

    // 提取实例覆盖材质色
    std::optional<QVector3D> instColor;
    if (!colorTool.IsNull()) {
        Quantity_ColorRGBA cRGBA;
        if (colorTool->GetColor(L, XCAFDoc_ColorGen, cRGBA) ||
            colorTool->GetColor(L, XCAFDoc_ColorSurf, cRGBA)) {
            float r = static_cast<float>(cRGBA.GetRGB().Red());
            float g = static_cast<float>(cRGBA.GetRGB().Green());
            float b = static_cast<float>(cRGBA.GetRGB().Blue());
            if (isValidCadColor(r, g, b)) {
                instColor = QVector3D(r, g, b);
            }
        }
    }
    if (!instColor.has_value()) {
        instColor = prototypes[protoId].defaultColor;
    }

    int nextPartId = static_cast<int>(instances.size() + 1);
    instances.push_back({ nextPartId, instName, protoId, currentLoc, instColor });
}

bool ModelLoaderWorker::parseSTEP(const QString& path, ModelDataPtr outModel) {
    traceWorkerLog("parseSTEP (OCCT XCAF): Loading " + path);
    emit sigProgress(15, "正在通过 OpenCASCADE 内核读取 CAD 几何与材质属性数据...");

    // 合作式等待获取全局 OCCT 单例互斥锁 (杜绝多线程并发竞态)
    std::unique_lock<std::mutex> lock(g_occtSessionMutex, std::defer_lock);
    while (!lock.try_lock()) {
        if (m_cancelRequested.load()) return false;
        QThread::msleep(20);
    }

    std::filesystem::path fsPath(path.toStdWString());
    std::ifstream stream(fsPath, std::ios::in | std::ios::binary);
    if (!stream.is_open()) {
        traceWorkerLog("parseSTEP: Failed to open file stream: " + path);
        return false;
    }

    // RAII 保护 XCAF 文档，确保异常或取消时自动执行 Close
    XcafDocGuard docGuard;

    try {
        XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", docGuard.doc);

        STEPCAFControl_Reader reader;
        reader.SetColorMode(true);
        reader.SetNameMode(true);
        reader.SetLayerMode(true);
        reader.SetPropsMode(true);

        IFSelect_ReturnStatus status = reader.ReadStream(fsPath.filename().string().c_str(), stream);
        if (status != IFSelect_RetDone) {
            traceWorkerLog(QString("parseSTEP: reader.ReadStream failed with status %1").arg(status));
            return false;
        }

        if (m_cancelRequested.load()) return false;

        emit sigProgress(45, "正在转换 CAD B-Rep 实体装配拓扑结构与属性树...");
        occ::handle<OcctCancelIndicator> stepCancelIndicator = new OcctCancelIndicator(m_cancelRequested);
        reader.Transfer(docGuard.doc, stepCancelIndicator->Start());
        if (m_cancelRequested.load()) return false;

        occ::handle<XCAFDoc_ShapeTool> shapeTool = XCAFDoc_DocumentTool::ShapeTool(docGuard.doc->Main());
        occ::handle<XCAFDoc_ColorTool> colorTool = XCAFDoc_DocumentTool::ColorTool(docGuard.doc->Main());

        // 递归遍历 XCAF 装配树提取原型与实例
        TDF_LabelSequence freeShapes;
        shapeTool->GetFreeShapes(freeShapes);

        std::vector<StepPrototype> prototypes;
        std::unordered_map<std::string, int> entryToProtoId;
        std::vector<StepInstance> instances;
        std::unordered_set<std::string> recursionGuard;

        for (int i = 1; i <= freeShapes.Length(); ++i) {
            walkAssemblyTree(shapeTool, colorTool, freeShapes.Value(i), TopLoc_Location(),
                             prototypes, entryToProtoId, instances, recursionGuard);
        }

        // 双重防御：若装配树仅产出 <=1 个组件，检查 OneShape 内部是否包含多个独立 Solid
        if (instances.size() <= 1) {
            TopoDS_Shape oneShape = shapeTool->GetOneShape();
            if (oneShape.IsNull()) oneShape = reader.Reader().OneShape();
            if (!oneShape.IsNull()) {
                int solidCount = 0;
                for (TopExp_Explorer exp(oneShape, TopAbs_SOLID); exp.More(); exp.Next()) {
                    solidCount++;
                }
                if (solidCount > 1) {
                    prototypes.clear();
                    entryToProtoId.clear();
                    instances.clear();
                    int sIdx = 1;
                    for (TopExp_Explorer exp(oneShape, TopAbs_SOLID); exp.More(); exp.Next()) {
                        const TopoDS_Shape& solid = exp.Current();
                        QString sName = QString("实体_%1").arg(sIdx);
                        std::optional<QVector3D> sCol;
                        if (!colorTool.IsNull()) {
                            Quantity_ColorRGBA cRGBA;
                            if (colorTool->GetColor(solid, XCAFDoc_ColorGen, cRGBA) ||
                                colorTool->GetColor(solid, XCAFDoc_ColorSurf, cRGBA)) {
                                float r = static_cast<float>(cRGBA.GetRGB().Red());
                                float g = static_cast<float>(cRGBA.GetRGB().Green());
                                float b = static_cast<float>(cRGBA.GetRGB().Blue());
                                if (isValidCadColor(r, g, b)) {
                                    sCol = QVector3D(r, g, b);
                                }
                            }
                        }
                        int pId = static_cast<int>(prototypes.size());
                        prototypes.push_back({ pId, TDF_Label(), solid, sName, sCol });
                        instances.push_back({ sIdx, sName, pId, TopLoc_Location(), sCol });
                        sIdx++;
                    }
                }
            }
        }

        traceWorkerLog(QString("parseSTEP: Discovered %1 instances across %2 prototypes via XCAF")
                       .arg(instances.size()).arg(prototypes.size()));

        bool result = false;
        if (instances.size() > 1) {
            // 原型单次剖分与实例展开
            emit sigProgress(50, QString("正在离散化装配体原型 (共 %1 种原型结构)...").arg(prototypes.size()));
            std::vector<QVector<SubMesh>> protoMeshes(prototypes.size());
            size_t totalTriangles = 0;
            double deflectionScale = 1.0;

            for (size_t pIdx = 0; pIdx < prototypes.size(); ++pIdx) {
                if (m_cancelRequested.load()) break;
                const auto& proto = prototypes[pIdx];

                // Triangle Budget 防护：若累计三角面数超过 3,000,000 面，自适应扩大公差防止超大装配体爆显存
                if (totalTriangles > 3000000) {
                    deflectionScale = 2.0;
                }

                int pct = 50 + static_cast<int>((pIdx + 1) * 35 / prototypes.size());
                emit sigProgress(pct, QString("正在剖分零件原型 [%1/%2]: %3")
                                 .arg(pIdx + 1).arg(prototypes.size()).arg(proto.baseName));

                try {
                    protoMeshes[pIdx] = tessellatePrototype(proto.localShape, proto.baseName,
                                                            colorTool, proto.defaultColor,
                                                            proto.protoId, deflectionScale);
                } catch (const Standard_Failure& e) {
                    traceWorkerLog(QString("parseSTEP: Prototype %1 '%2' tessellation caught Standard_Failure: %3")
                                   .arg(pIdx).arg(proto.baseName).arg(e.GetMessageString()));
                } catch (const std::exception& e) {
                    traceWorkerLog(QString("parseSTEP: Prototype %1 '%2' tessellation caught std::exception: %3")
                                   .arg(pIdx).arg(proto.baseName).arg(e.what()));
                } catch (...) {
                    traceWorkerLog(QString("parseSTEP: Prototype %1 '%2' tessellation caught unknown exception")
                                   .arg(pIdx).arg(proto.baseName));
                }

                for (const auto& m : protoMeshes[pIdx]) {
                    if (!m.isFeatureEdge) {
                        totalTriangles += m.indices.size() / 3;
                    }
                }
            }

            if (m_cancelRequested.load()) return false;

            emit sigProgress(85, QString("正在组装空间装配体部件 (共 %1 个实例)...").arg(instances.size()));
            for (size_t i = 0; i < instances.size(); ++i) {
                if (m_cancelRequested.load()) break;
                const auto& inst = instances[i];
                if (inst.protoId < 0 || inst.protoId >= static_cast<int>(protoMeshes.size())) continue;

                const auto& meshes = protoMeshes[inst.protoId];
                QMatrix4x4 instMat = occLocToQMatrix(inst.location);

                for (const auto& baseMesh : meshes) {
                    SubMesh instMesh = baseMesh; // Qt QVector 隐式共享，极速浅拷贝
                    instMesh.name = baseMesh.isFeatureEdge ? (inst.name + "_edges") : inst.name;
                    instMesh.partId = inst.partId;
                    instMesh.transform = instMat;
                    if (inst.partColor.has_value() && !baseMesh.isFeatureEdge && !baseMesh.hasVertexColors) {
                        instMesh.diffuseColor = inst.partColor.value();
                    }
                    outModel->meshes.push_back(std::move(instMesh));
                }
            }
            result = !outModel->meshes.isEmpty();
        } else {
            // 单实体零件模式：极速单体剖分
            TopoDS_Shape shape = shapeTool->GetOneShape();
            if (shape.IsNull()) {
                shape = reader.Reader().OneShape();
            }
            if (shape.IsNull() && !prototypes.empty()) {
                shape = prototypes[0].localShape;
            }
            if (shape.IsNull()) {
                traceWorkerLog("parseSTEP: null shape obtained from STEP");
                return false;
            }

            QString partName = QFileInfo(path).baseName();
            std::optional<QVector3D> pCol;
            if (!instances.empty()) {
                partName = instances[0].name;
                pCol = instances[0].partColor;
            }
            result = processShapeTessellation(shape, partName, outModel, colorTool, 1, pCol);
        }

        return result;
    } catch (const Standard_Failure& e) {
        traceWorkerLog(QString("parseSTEP: OCCT Standard_Failure caught: %1").arg(e.GetMessageString()));
        return false;
    } catch (const std::exception& e) {
        traceWorkerLog(QString("parseSTEP: std::exception caught: %1").arg(e.what()));
        return false;
    } catch (...) {
        traceWorkerLog("parseSTEP: Unknown critical exception caught in CAD kernel!");
        return false;
    }
}

bool ModelLoaderWorker::parseIGES(const QString& path, ModelDataPtr outModel) {
    traceWorkerLog("parseIGES (OCCT): Loading " + path);
    emit sigProgress(15, "正在通过 OpenCASCADE 内核读取 IGES 几何数据...");

    std::unique_lock<std::mutex> lock(g_occtSessionMutex, std::defer_lock);
    while (!lock.try_lock()) {
        if (m_cancelRequested.load()) return false;
        QThread::msleep(20);
    }

    try {
        IGESControl_Reader reader;
        // OCCT IGES 解析器原生调用 ReadFile 结合安全路径别名机制
        QString safePath = resolveSafePath(path);
        IFSelect_ReturnStatus status = reader.ReadFile(safePath.toLocal8Bit().constData());
        if (status != IFSelect_RetDone) {
            status = reader.ReadFile(path.toLocal8Bit().constData());
        }
        if (status != IFSelect_RetDone) {
            traceWorkerLog(QString("parseIGES: reader.ReadFile failed with status %1").arg(status));
            return false;
        }

        if (m_cancelRequested.load()) return false;

        emit sigProgress(40, "正在转换 IGES 几何实体与装配 (TransferRoots)...");
        occ::handle<OcctCancelIndicator> igesCancelIndicator = new OcctCancelIndicator(m_cancelRequested);
        reader.TransferRoots(igesCancelIndicator->Start());
        if (m_cancelRequested.load()) return false;

        TopoDS_Shape shape = reader.OneShape();
        if (shape.IsNull()) {
            traceWorkerLog("parseIGES: reader.OneShape() returned null shape");
            return false;
        }

        if (m_cancelRequested.load()) return false;

        // IGES 模型常见破面与未缝合问题，通过 ShapeFix 进行强力几何自愈
        emit sigProgress(55, "正在执行 IGES 拓扑自愈与曲面缝合 (ShapeFix)...");
        ShapeFix_Shape fixer(shape);
        fixer.Perform(igesCancelIndicator->Start());
        if (m_cancelRequested.load()) return false;
        shape = fixer.Shape();

        return processShapeTessellation(shape, QFileInfo(path).baseName(), outModel);
    } catch (const Standard_Failure& e) {
        traceWorkerLog(QString("parseIGES: OCCT Standard_Failure caught: %1").arg(e.GetMessageString()));
        return false;
    } catch (const std::exception& e) {
        traceWorkerLog(QString("parseIGES: std::exception caught: %1").arg(e.what()));
        return false;
    } catch (...) {
        traceWorkerLog("parseIGES: Unknown critical exception caught in CAD kernel!");
        return false;
    }
}

bool ModelLoaderWorker::parseGLTF(const QString& path, ModelDataPtr outModel) {
    traceWorkerLog("parseGLTF (OCCT): Loading " + path);
    emit sigProgress(15, "正在通过 OpenCASCADE DEGLTF 内核读取 glTF/GLB 几何数据...");

    std::unique_lock<std::mutex> lock(g_occtSessionMutex, std::defer_lock);
    while (!lock.try_lock()) {
        if (m_cancelRequested.load()) return false;
        QThread::msleep(20);
    }

    try {
        QString safePath = resolveSafePath(path);
        occ::handle<DEGLTF_ConfigurationNode> aNode = new DEGLTF_ConfigurationNode();
        // glTF 官方标准采用 Y-up 坐标系，配置视口坐标系对齐
        aNode->InternalParameters.SystemCS = RWMesh_CoordinateSystem_Yup;
        aNode->InternalParameters.FileCS = RWMesh_CoordinateSystem_Yup;

        DEGLTF_Provider provider(aNode);
        TopoDS_Shape shape;

        QByteArray utf8Path = safePath.toUtf8();
        bool ok = provider.Read(TCollection_AsciiString(utf8Path.constData()), shape);
        if (!ok || shape.IsNull()) {
            // 回退使用原始路径重试
            utf8Path = path.toUtf8();
            ok = provider.Read(TCollection_AsciiString(utf8Path.constData()), shape);
        }

        if (!ok || shape.IsNull()) {
            traceWorkerLog("parseGLTF: provider.Read failed or shape is null!");
            return false;
        }

        if (m_cancelRequested.load()) return false;

        emit sigProgress(50, "正在提取 glTF 网格与材质特征...");
        return processShapeTessellation(shape, QFileInfo(path).baseName(), outModel);
    } catch (const Standard_Failure& e) {
        traceWorkerLog(QString("parseGLTF: OCCT Standard_Failure caught: %1").arg(e.GetMessageString()));
        return false;
    } catch (const std::exception& e) {
        traceWorkerLog(QString("parseGLTF: std::exception caught: %1").arg(e.what()));
        return false;
    } catch (...) {
        traceWorkerLog("parseGLTF: Unknown critical exception caught in glTF kernel!");
        return false;
    }
}

bool ModelLoaderWorker::parse3MF(const QString& path, ModelDataPtr outModel) {
    traceWorkerLog("parse3MF: Loading " + path);
    emit sigProgress(15, "正在解压缩 3MF 制造模型包 (QZipReader)...");

    std::unique_ptr<QZipReader> zip = std::make_unique<QZipReader>(path);
    if (!zip->isReadable()) {
        QString safePath = resolveSafePath(path);
        zip = std::make_unique<QZipReader>(safePath);
        if (!zip->isReadable()) {
            traceWorkerLog("parse3MF: Failed to open 3MF ZIP archive: " + path);
            return false;
        }
    }

    // 寻找 3D/3dmodel.model 或任意 .model 入口
    QString modelEntry = "3D/3dmodel.model";
    bool found = false;
    const auto entries = zip->fileInfoList();
    for (const auto& entry : entries) {
        if (entry.filePath.compare("3D/3dmodel.model", Qt::CaseInsensitive) == 0) {
            modelEntry = entry.filePath;
            found = true;
            break;
        }
    }
    if (!found) {
        for (const auto& entry : entries) {
            if (entry.filePath.endsWith(".model", Qt::CaseInsensitive)) {
                modelEntry = entry.filePath;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        traceWorkerLog("parse3MF: No .model file found in 3MF archive!");
        return false;
    }

    QByteArray modelXmlData = zip->fileData(modelEntry);
    zip->close();

    if (modelXmlData.isEmpty()) {
        traceWorkerLog("parse3MF: Model XML data is empty!");
        return false;
    }

    emit sigProgress(30, "正在流式解析 3MF 几何网格与装配结构 (QXmlStreamReader)...");

    struct RawObject {
        int id = 0;
        QString name;
        int pid = -1;
        int pindex = -1;
        QVector<QVector3D> vertices;
        struct Tri {
            int v1 = 0, v2 = 0, v3 = 0;
            int pid = -1;
            int p1 = -1;
        };
        QVector<Tri> triangles;
        struct Component {
            int objectId = 0;
            QMatrix4x4 transform;
        };
        QVector<Component> components;
    };

    struct BuildItem {
        int objectId = 0;
        QMatrix4x4 transform;
    };

    auto parse3MFTransform = [](const QString& str) -> QMatrix4x4 {
        QMatrix4x4 m;
        m.setToIdentity();
        if (str.isEmpty()) return m;
        QStringList parts = str.split(' ', Qt::SkipEmptyParts);
        if (parts.size() >= 12) {
            float m00 = parts[0].toFloat();
            float m01 = parts[1].toFloat();
            float m02 = parts[2].toFloat();
            float m10 = parts[3].toFloat();
            float m11 = parts[4].toFloat();
            float m12 = parts[5].toFloat();
            float m20 = parts[6].toFloat();
            float m21 = parts[7].toFloat();
            float m22 = parts[8].toFloat();
            float m30 = parts[9].toFloat();
            float m31 = parts[10].toFloat();
            float m32 = parts[11].toFloat();
            m = QMatrix4x4(
                m00, m10, m20, m30,
                m01, m11, m21, m31,
                m02, m12, m22, m32,
                0.0f, 0.0f, 0.0f, 1.0f
            );
        }
        return m;
    };

    auto parseHexColor = [](const QString& str) -> QColor {
        QString s = str.trimmed();
        if (s.startsWith('#')) s.remove(0, 1);
        if (s.length() == 6) {
            bool ok = false;
            unsigned int val = s.toUInt(&ok, 16);
            if (ok) {
                return QColor((val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF);
            }
        } else if (s.length() == 8) {
            bool ok = false;
            unsigned int val = s.toUInt(&ok, 16);
            if (ok) {
                return QColor((val >> 24) & 0xFF, (val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF);
            }
        }
        return QColor(220, 120, 80);
    };

    QMap<int, RawObject> objects;
    QMap<int, QVector<QColor>> propertyColors;
    QVector<BuildItem> buildItems;
    float unitScale = 1.0f; // 毫米 (mm) 标准基准

    QXmlStreamReader xml(modelXmlData);
    RawObject curObj;
    bool inObject = false;
    bool inMesh = false;
    bool inVertices = false;
    bool inTriangles = false;
    int curColorGroupId = -1;

    while (!xml.atEnd() && !xml.hasError()) {
        if (m_cancelRequested.load()) return false;
        auto token = xml.readNext();

        if (token == QXmlStreamReader::StartElement) {
            auto name = xml.name();
            if (name == QLatin1String("model")) {
                QString unit = xml.attributes().value("unit").toString().toLower();
                if (unit == "micron") unitScale = 0.001f;
                else if (unit == "millimeter" || unit.isEmpty()) unitScale = 1.0f;
                else if (unit == "centimeter") unitScale = 10.0f;
                else if (unit == "meter") unitScale = 1000.0f;
                else if (unit == "inch") unitScale = 25.4f;
                else if (unit == "foot") unitScale = 304.8f;
            } else if (name == QLatin1String("colorgroup") || name == QLatin1String("basematerials")) {
                curColorGroupId = xml.attributes().value("id").toInt();
            } else if (name == QLatin1String("color") && curColorGroupId >= 0) {
                QString cStr = xml.attributes().value("color").toString();
                if (!cStr.isEmpty()) {
                    propertyColors[curColorGroupId].push_back(parseHexColor(cStr));
                }
            } else if (name == QLatin1String("base") && curColorGroupId >= 0) {
                QString cStr = xml.attributes().value("displaycolor").toString();
                if (!cStr.isEmpty()) {
                    propertyColors[curColorGroupId].push_back(parseHexColor(cStr));
                }
            } else if (name == QLatin1String("object")) {
                inObject = true;
                curObj = RawObject();
                curObj.id = xml.attributes().value("id").toInt();
                curObj.name = xml.attributes().value("name").toString();
                if (xml.attributes().hasAttribute("pid")) {
                    curObj.pid = xml.attributes().value("pid").toInt();
                }
                if (xml.attributes().hasAttribute("pindex")) {
                    curObj.pindex = xml.attributes().value("pindex").toInt();
                }
            } else if (name == QLatin1String("mesh") && inObject) {
                inMesh = true;
            } else if (name == QLatin1String("vertices") && inMesh) {
                inVertices = true;
            } else if (name == QLatin1String("vertex") && inVertices) {
                float x = xml.attributes().value("x").toFloat() * unitScale;
                float y = xml.attributes().value("y").toFloat() * unitScale;
                float z = xml.attributes().value("z").toFloat() * unitScale;
                curObj.vertices.push_back(QVector3D(x, y, z));
            } else if (name == QLatin1String("triangles") && inMesh) {
                inTriangles = true;
            } else if (name == QLatin1String("triangle") && inTriangles) {
                RawObject::Tri tri;
                tri.v1 = xml.attributes().value("v1").toInt();
                tri.v2 = xml.attributes().value("v2").toInt();
                tri.v3 = xml.attributes().value("v3").toInt();
                if (xml.attributes().hasAttribute("pid")) {
                    tri.pid = xml.attributes().value("pid").toInt();
                }
                if (xml.attributes().hasAttribute("p1")) {
                    tri.p1 = xml.attributes().value("p1").toInt();
                }
                curObj.triangles.push_back(tri);
            } else if (name == QLatin1String("component") && inObject) {
                RawObject::Component comp;
                comp.objectId = xml.attributes().value("objectid").toInt();
                comp.transform = parse3MFTransform(xml.attributes().value("transform").toString());
                curObj.components.push_back(comp);
            } else if (name == QLatin1String("item")) {
                BuildItem item;
                item.objectId = xml.attributes().value("objectid").toInt();
                item.transform = parse3MFTransform(xml.attributes().value("transform").toString());
                buildItems.push_back(item);
            }
        } else if (token == QXmlStreamReader::EndElement) {
            auto name = xml.name();
            if (name == QLatin1String("colorgroup") || name == QLatin1String("basematerials")) {
                curColorGroupId = -1;
            } else if (name == QLatin1String("object")) {
                inObject = false;
                objects[curObj.id] = curObj;
            } else if (name == QLatin1String("mesh")) {
                inMesh = false;
            } else if (name == QLatin1String("vertices")) {
                inVertices = false;
            } else if (name == QLatin1String("triangles")) {
                inTriangles = false;
            }
        }
    }

    if (buildItems.isEmpty()) {
        // 若没有明确声明 <build>，默认将所有包含网格的 object 加入渲染
        for (auto it = objects.begin(); it != objects.end(); ++it) {
            if (!it.value().triangles.isEmpty() || !it.value().components.isEmpty()) {
                BuildItem item;
                item.objectId = it.key();
                buildItems.push_back(item);
            }
        }
    }

    emit sigProgress(60, "正在计算 3MF 装配体原型与空间变换 (Prototype Instancing)...");

    // 空间哈希顶点焊接与曲面平滑结构体
    struct PosKey {
        int64_t x, y, z;
        bool operator==(const PosKey& o) const {
            return x == o.x && y == o.y && z == o.z;
        }
    };
    struct PosKeyHash {
        size_t operator()(const PosKey& k) const {
            size_t h1 = std::hash<int64_t>{}(k.x);
            size_t h2 = std::hash<int64_t>{}(k.y);
            size_t h3 = std::hash<int64_t>{}(k.z);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };
    struct VertexCandidate {
        uint32_t vertexIndex;
        QVector3D refNormal;
        QVector4D color;
    };

    struct EdgeKey {
        int64_t u, v;
        bool operator==(const EdgeKey& o) const {
            return u == o.u && v == o.v;
        }
    };
    struct EdgeKeyHash {
        size_t operator()(const EdgeKey& k) const {
            return std::hash<int64_t>{}(k.u) ^ (std::hash<int64_t>{}(k.v) << 1);
        }
    };

    // 第一步：对所有包含网格的 RawObject 执行单次局部空间原型剖分与特征线提取
    QMap<int, QVector<SubMesh>> protoMeshes;
    for (auto it = objects.begin(); it != objects.end(); ++it) {
        if (m_cancelRequested.load()) return false;
        const RawObject& obj = it.value();
        if (obj.triangles.isEmpty() || obj.vertices.isEmpty()) continue;

        SubMesh solidMesh;
        solidMesh.name = obj.name.isEmpty() ? QString("3MF_Part_%1").arg(obj.id) : obj.name;
        solidMesh.primitiveType = 0x0004; // GL_TRIANGLES
        solidMesh.prototypeId = obj.id;

        // 默认 3D 打印工业高分子耗材色 (珊瑚橙或优雅炭黑)
        QVector3D defaultColor(0.85f, 0.45f, 0.25f);
        if (obj.pid >= 0 && propertyColors.contains(obj.pid) && obj.pindex >= 0) {
            const auto& cols = propertyColors[obj.pid];
            if (obj.pindex < cols.size()) {
                const QColor& c = cols[obj.pindex];
                defaultColor = QVector3D(c.redF(), c.greenF(), c.blueF());
            }
        }
        solidMesh.diffuseColor = defaultColor;

        std::unordered_map<PosKey, std::vector<VertexCandidate>, PosKeyHash> posMap;
        std::vector<QVector3D> accumulatedNormals;

        auto addWeldedVertex = [&](const QVector3D& pos, const QVector3D& normal, const QVector4D& col) -> uint32_t {
            PosKey key = { std::llround(pos.x() * 1000.0), std::llround(pos.y() * 1000.0), std::llround(pos.z() * 1000.0) };
            auto& cands = posMap[key];
            for (const auto& cand : cands) {
                float cosAngle = QVector3D::dotProduct(cand.refNormal, normal);
                if (cosAngle >= 0.707f) { // 45 度阈值保锐角平滑
                    accumulatedNormals[cand.vertexIndex] += normal;
                    return cand.vertexIndex;
                }
            }

            uint32_t newIdx = static_cast<uint32_t>(solidMesh.vertices.size());
            solidMesh.vertices.push_back(Vertex(pos, normal, QVector2D(), col));
            accumulatedNormals.push_back(normal);
            cands.push_back({newIdx, normal, col});
            return newIdx;
        };

        std::unordered_map<EdgeKey, std::vector<QVector3D>, EdgeKeyHash> edgeMap;
        std::unordered_map<EdgeKey, std::pair<QVector3D, QVector3D>, EdgeKeyHash> edgePoints;

        for (const auto& tri : obj.triangles) {
            if (tri.v1 < 0 || tri.v1 >= obj.vertices.size() ||
                tri.v2 < 0 || tri.v2 >= obj.vertices.size() ||
                tri.v3 < 0 || tri.v3 >= obj.vertices.size()) {
                continue;
            }

            // 局部坐标系点
            const QVector3D& p1 = obj.vertices[tri.v1];
            const QVector3D& p2 = obj.vertices[tri.v2];
            const QVector3D& p3 = obj.vertices[tri.v3];

            QVector3D fn = QVector3D::crossProduct(p2 - p1, p3 - p1).normalized();
            if (fn.isNull()) fn = QVector3D(0, 1, 0);

            // 三角形独立材质色判断
            QVector4D triCol(defaultColor.x(), defaultColor.y(), defaultColor.z(), 1.0f);
            int triPid = (tri.pid >= 0) ? tri.pid : obj.pid;
            int triPindex = (tri.p1 >= 0) ? tri.p1 : obj.pindex;
            if (triPid >= 0 && propertyColors.contains(triPid) && triPindex >= 0) {
                const auto& cols = propertyColors[triPid];
                if (triPindex < cols.size()) {
                    const QColor& c = cols[triPindex];
                    triCol = QVector4D(c.redF(), c.greenF(), c.blueF(), 1.0f);
                    solidMesh.hasVertexColors = true;
                }
            }

            uint32_t i1 = addWeldedVertex(p1, fn, triCol);
            uint32_t i2 = addWeldedVertex(p2, fn, triCol);
            uint32_t i3 = addWeldedVertex(p3, fn, triCol);

            solidMesh.indices.push_back(i1);
            solidMesh.indices.push_back(i2);
            solidMesh.indices.push_back(i3);

            // 记录 3 条边供特征硬棱线提取
            auto registerEdge = [&](int vA, int vB, const QVector3D& ptA, const QVector3D& ptB) {
                int64_t k1 = std::min(vA, vB);
                int64_t k2 = std::max(vA, vB);
                EdgeKey ek = {k1, k2};
                edgeMap[ek].push_back(fn);
                edgePoints[ek] = {ptA, ptB};
            };
            registerEdge(tri.v1, tri.v2, p1, p2);
            registerEdge(tri.v2, tri.v3, p2, p3);
            registerEdge(tri.v3, tri.v1, p3, p1);
        }

        // 归一化所有累积法向量
        for (size_t i = 0; i < solidMesh.vertices.size(); ++i) {
            QVector3D n = accumulatedNormals[i].normalized();
            if (!n.isNull()) {
                solidMesh.vertices[i].normal = n;
            }
        }

        solidMesh.updateLocalAabb();

        QVector<SubMesh> objMeshes;
        if (!solidMesh.vertices.isEmpty()) {
            objMeshes.push_back(solidMesh);
        }

        // 构建 CAD 特征硬轮廓线 (Feature Edges)
        SubMesh edgeMesh;
        edgeMesh.name = solidMesh.name + "_edges";
        edgeMesh.diffuseColor = QVector3D(0.12f, 0.14f, 0.18f); // 深空炭黑
        edgeMesh.primitiveType = 0x0001; // GL_LINES
        edgeMesh.lineWidth = 1.5f;
        edgeMesh.isFeatureEdge = true;
        edgeMesh.prototypeId = obj.id;

        for (const auto& kv : edgeMap) {
            const auto& normalsList = kv.second;
            bool isFeature = false;
            if (normalsList.size() == 1) {
                // 开放边界轮廓线
                isFeature = true;
            } else if (normalsList.size() >= 2) {
                float dot = QVector3D::dotProduct(normalsList[0], normalsList[1]);
                if (dot < 0.88f) { // 夹角 > 28 度，为尖锐结构硬转折
                    isFeature = true;
                }
            }
            if (isFeature) {
                const auto& pts = edgePoints[kv.first];
                uint32_t idx = static_cast<uint32_t>(edgeMesh.vertices.size());
                edgeMesh.vertices.push_back(Vertex(pts.first));
                edgeMesh.vertices.push_back(Vertex(pts.second));
                edgeMesh.indices.push_back(idx);
                edgeMesh.indices.push_back(idx + 1);
            }
        }

        edgeMesh.updateLocalAabb();
        if (!edgeMesh.vertices.isEmpty()) {
            objMeshes.push_back(edgeMesh);
        }

        protoMeshes[obj.id] = std::move(objMeshes);
    }

    // 第二步：递归展开装配树生成轻量级实例 (零拷贝共享原型顶点与显存缓冲)
    struct StackGuard {
        std::unordered_set<int>& s;
        int id;
        bool active;
        StackGuard(std::unordered_set<int>& setRef, int i) : s(setRef), id(i), active(true) { s.insert(id); }
        ~StackGuard() { if (active) s.erase(id); }
    };
    std::unordered_set<int> recursionStack;
    int instanceCounter = 0;

    std::function<void(int, const QMatrix4x4&, int)> instantiateObject;
    instantiateObject = [&](int objId, const QMatrix4x4& parentTrsf, int depth) {
        if (depth > 64) return;
        if (recursionStack.count(objId)) return;
        StackGuard guard(recursionStack, objId);
        if (!objects.contains(objId)) return;
        const RawObject& obj = objects[objId];

        // 递归处理子组件
        for (const auto& comp : obj.components) {
            QMatrix4x4 childTrsf = parentTrsf * comp.transform;
            instantiateObject(comp.objectId, childTrsf, depth + 1);
        }

        if (!protoMeshes.contains(objId)) return;
        const auto& meshes = protoMeshes[objId];
        instanceCounter++;

        for (const auto& baseMesh : meshes) {
            SubMesh instMesh = baseMesh; // Qt 隐式共享零拷贝
            instMesh.partId = instanceCounter;
            instMesh.transform = parentTrsf;
            instMesh.name = baseMesh.isFeatureEdge ? QString("零件_%1_edges").arg(instanceCounter)
                                                   : (obj.name.isEmpty() ? QString("零件_%1").arg(instanceCounter) : obj.name);
            outModel->meshes.push_back(std::move(instMesh));
        }
    };

    for (const auto& item : buildItems) {
        if (m_cancelRequested.load()) return false;
        instantiateObject(item.objectId, item.transform, 0);
    }

    traceWorkerLog(QString("parse3MF: Finished loading %1 meshes across %2 prototypes via Prototype Instancing.")
                   .arg(outModel->meshes.size()).arg(protoMeshes.size()));
    return !outModel->meshes.isEmpty();
}

bool ModelLoaderWorker::parsePRT(const QString& path, ModelDataPtr outModel) {
    traceWorkerLog("parsePRT: Analyzing PRT binary header for " + path);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        traceWorkerLog("parsePRT: Failed to open file " + path);
        return false;
    }

    // 读取前 128KB 数据，以充分嗅探微软复合文档 (OLE2/CFBF) 目录扇区与传统 Unix/RISC 头部
    QByteArray header = file.read(131072);
    file.close();

    // 格式识别魔数嗅探
    bool isSiemensNX = false;
    bool isCreo = false;

    // 微软 OLE2 复合二进制文件特征魔数 (D0 CF 11 E0 A1 B1 1A E1)
    const char ole2Magic[] = "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1";
    const bool isOle2 = (header.size() >= 8 && memcmp(header.constData(), ole2Magic, 8) == 0);

    if (isOle2) {
        // 现代所有主流版本的 Siemens NX / UG PRT 文件均采用 OLE2 复合文档封装
        // 微软 CFBF 规范中，目录扇区存储的流名称为 UTF-16LE 编码；同时数据扇区可能包含 ASCII 流名或标识
        auto matchesStreamName = [&](const char* asciiName) -> bool {
            if (header.contains(asciiName)) return true;
            QByteArray utf16Bytes;
            for (int i = 0; asciiName[i] != '\0'; ++i) {
                utf16Bytes.append(asciiName[i]);
                utf16Bytes.append('\0');
            }
            return header.contains(utf16Bytes);
        };

        if (matchesStreamName("UgAttributes") || matchesStreamName("UGII") ||
            matchesStreamName("OM_root_object") || matchesStreamName("Siemens PLM") ||
            matchesStreamName("UG_PART") || matchesStreamName("NX_PART") ||
            matchesStreamName("UGPart") || matchesStreamName("NXPart")) {
            isSiemensNX = true;
        }
    } else if (header.contains("UGII") || header.contains("OM_root_object") ||
               (header.contains("hp7151") && header.contains("UG"))) {
        isSiemensNX = true;
    } else if (header.startsWith("#UGC:") || header.startsWith("#PRT") ||
               (header.contains("Pro/ENGINEER") && header.contains("PART"))) {
        isCreo = true;
    }

    QString cadVendor = isSiemensNX ? "西门子 UG / Siemens NX" : (isCreo ? "PTC Creo / Pro-E" : "CAD 原生零件");

    traceWorkerLog(QString("parsePRT: Identified vendor: %1 (isNX: %2, isCreo: %3)")
                   .arg(cadVendor).arg(isSiemensNX).arg(isCreo));

    QString detectedUgBaseDir;
    QString detectedLic;

    // 如果识别为西门子 UG/NX，尝试调用本机安装的 NX 静默转码引擎 (Local Headless Converter Bridge)
    if (isSiemensNX) {
        detectedUgBaseDir = qEnvironmentVariable("UGII_BASE_DIR");

#ifdef Q_OS_WIN
        // 1. 扫描 Windows 注册表中的真实 NX/Unigraphics 安装记录（覆盖 64位与 WOW6432 视图及各类供应商主键）
        if (detectedUgBaseDir.isEmpty() || !QDir(detectedUgBaseDir).exists()) {
            const QStringList regRoots = {
                "HKEY_LOCAL_MACHINE\\SOFTWARE\\Siemens\\NX",
                "HKEY_LOCAL_MACHINE\\SOFTWARE\\Siemens PLM Software\\NX",
                "HKEY_LOCAL_MACHINE\\SOFTWARE\\Unigraphics Solutions\\NX",
                "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Siemens\\NX",
                "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Siemens PLM Software\\NX",
                "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Unigraphics Solutions\\NX"
            };
            for (const QString& root : regRoots) {
                QSettings settings(root, QSettings::NativeFormat);
                // 先尝试读取本键下直接配置的路径
                QString directDir = settings.value("UGII_BASE_DIR").toString();
                if (directDir.isEmpty()) directDir = settings.value("INSTALLDIR").toString();
                if (!directDir.isEmpty()) {
                    directDir = QDir::fromNativeSeparators(directDir);
                    while (directDir.endsWith('/')) directDir.chop(1);
                    if (QDir(directDir).exists() && QFile::exists(directDir + "/STEP214UG/step214ug.exe")) {
                        detectedUgBaseDir = directDir;
                        if (detectedLic.isEmpty()) detectedLic = settings.value("SPLM_LICENSE_SERVER").toString();
                        if (detectedLic.isEmpty()) detectedLic = settings.value("LICENSESERVER").toString();
                        break;
                    }
                }

                // 再遍历各版本子键
                const QStringList versions = settings.childGroups();
                for (const QString& ver : versions) {
                    settings.beginGroup(ver);
                    QString dir = settings.value("UGII_BASE_DIR").toString();
                    if (dir.isEmpty()) dir = settings.value("INSTALLDIR").toString();
                    QString candLic = settings.value("SPLM_LICENSE_SERVER").toString();
                    if (candLic.isEmpty()) candLic = settings.value("LICENSESERVER").toString();
                    settings.endGroup();

                    if (!dir.isEmpty()) {
                        dir = QDir::fromNativeSeparators(dir);
                        while (dir.endsWith('/')) dir.chop(1);
                        if (QDir(dir).exists() && QFile::exists(dir + "/STEP214UG/step214ug.exe")) {
                            detectedUgBaseDir = dir;
                            if (detectedLic.isEmpty() && !candLic.isEmpty()) {
                                detectedLic = candLic;
                            }
                            break;
                        }
                    }
                }
                if (!detectedUgBaseDir.isEmpty()) break;
            }
        }
#endif

        // 2. 多盘符常见安装目录扫描保底 (覆盖主流 NX 8.5 到 NX 2412 各版本)
        if (detectedUgBaseDir.isEmpty() || !QDir(detectedUgBaseDir).exists()) {
            const QStringList drives = { "C:", "D:", "E:", "F:" };
            const QStringList subPaths = {
                "/Program Files/Siemens/NX 10.0", "/Program Files/Siemens/NX 11.0",
                "/Program Files/Siemens/NX 12.0", "/Program Files/Siemens/NX",
                "/Program Files/Siemens/NX2406",  "/Program Files/Siemens/NX2412",
                "/Program Files/Siemens/NX 2312", "/Program Files/Siemens/NX 2212",
                "/Program Files/Siemens/NX 2007", "/Program Files/Siemens/NX 1980",
                "/Siemens/NX 10.0",               "/Siemens/NX 12.0",
                "/Siemens/NX",                    "/Siemens/NX2406"
            };
            for (const QString& drive : drives) {
                for (const QString& sub : subPaths) {
                    QString cand = drive + sub;
                    if (QDir(cand).exists() && QFile::exists(cand + "/STEP214UG/step214ug.exe")) {
                        detectedUgBaseDir = cand;
                        break;
                    }
                }
                if (!detectedUgBaseDir.isEmpty()) break;
            }
        }

        QString translatorExe = detectedUgBaseDir + "/STEP214UG/step214ug.exe";
        if (!detectedUgBaseDir.isEmpty() && QFile::exists(translatorExe)) {
            traceWorkerLog("parsePRT: Found local NX installation at: " + detectedUgBaseDir);

            // 构造磁盘持久化缓存目录 (使用标准系统 Temp 路径，规避 UWP 本地虚拟化与超长路径限制)
            QString cacheDir = QDir::tempPath() + "/3dmaster_prt_cache";
            QDir().mkpath(cacheDir);

            // 基于文件完整绝对路径、尺寸与修改时间生成唯一特征哈希
            QFileInfo fi(path);
            QString absPath = fi.absoluteFilePath();
            QString keyStr = QString("%1_%2_%3")
                                 .arg(absPath)
                                 .arg(fi.size())
                                 .arg(fi.lastModified().toMSecsSinceEpoch());
            QString hashKey = QString::fromLatin1(QCryptographicHash::hash(keyStr.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
            // 采用纯 ASCII 安全文件名，杜绝 ANSI 命令行、特殊符号与旧版转换器编码乱码
            QString cachedStpPath = cacheDir + "/ug_" + hashKey + ".stp";

            bool stpReady = false;
            // 校验已有缓存是否完整有效 (严格校验 ISO-10303-21 标头)
            if (QFile::exists(cachedStpPath)) {
                bool cacheValid = false;
                if (QFileInfo(cachedStpPath).size() > 0) {
                    QFile checkStp(cachedStpPath);
                    if (checkStp.open(QIODevice::ReadOnly)) {
                        QByteArray stpHead = checkStp.read(1024);
                        checkStp.close();
                        if (stpHead.contains("ISO-10303-21")) {
                            cacheValid = true;
                        }
                    }
                }
                if (cacheValid) {
                    traceWorkerLog("parsePRT: Found existing valid cached STEP translation: " + cachedStpPath);
                    stpReady = true;
                } else {
                    traceWorkerLog("parsePRT: Found corrupted cached STEP file, purging: " + cachedStpPath);
                    QFile::remove(cachedStpPath);
                }
            }

            if (!stpReady) {
                emit sigProgress(15, "检测到西门子 UG 模型，正在调用本机 NX 引擎静默转码...");

                // 并发保护：转码过程使用独立 PID + 随机后缀的临时文件，消除同键并发写冲突
                quint32 rndVal = QRandomGenerator::global()->generate();
                QString tempStpPath = QString("%1/ug_%2_%3_%4.tmp")
                                          .arg(cacheDir, hashKey)
                                          .arg(QCoreApplication::applicationPid())
                                          .arg(rndVal);

                QProcess proc;
                QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
                env.insert("UGII_BASE_DIR", QDir::toNativeSeparators(detectedUgBaseDir));
                env.insert("STEP214UG_DIR", QDir::toNativeSeparators(detectedUgBaseDir + "/STEP214UG/"));
                env.insert("ROSE_DB", QDir::toNativeSeparators(detectedUgBaseDir + "/STEP214UG/"));
                env.insert("ROSE", QDir::toNativeSeparators(detectedUgBaseDir + "/STEP214UG/"));
                env.insert("UGII_ROOT_DIR", QDir::toNativeSeparators(detectedUgBaseDir + "/UGII/"));

                // 加固: 注入 ugii_env.dat 环境定义文件
                QString envDat = detectedUgBaseDir + "/UGII/ugii_env.dat";
                if (QFile::exists(envDat)) {
                    env.insert("UGII_ENV_FILE", QDir::toNativeSeparators(envDat));
                }

                // 加固: 全版本 Siemens 许可证环境变量齐备并按类型严格路由
                // 1. 注册表与准入目录配对出的许可证优先于散落环境变量
                QString lic = detectedLic;
                if (lic.isEmpty()) lic = qEnvironmentVariable("SPLM_LICENSE_SERVER");
                if (lic.isEmpty()) lic = qEnvironmentVariable("UGS_LICENSE_SERVER");
                if (lic.isEmpty()) lic = qEnvironmentVariable("UGII_LICENSE_FILE");
                if (lic.isEmpty() && QFile::exists("C:/ProgramData/Siemens/siemens_SSQ.dat")) {
                    lic = "C:\\ProgramData\\Siemens\\siemens_SSQ.dat";
                }
                if (!lic.isEmpty()) {
                    if (lic.contains('@')) {
                        // 端口@主机 格式：服务型变量
                        env.insert("SPLM_LICENSE_SERVER", lic);
                        env.insert("UGS_LICENSE_SERVER", lic);
                        env.remove("UGII_LICENSE_FILE");
                    } else {
                        // 节点锁定文件路径：仅写入文件型变量，清理服务型变量
                        env.insert("UGII_LICENSE_FILE", lic);
                        env.remove("SPLM_LICENSE_SERVER");
                        env.remove("UGS_LICENSE_SERVER");
                    }
                }

                // 加固: PATH 纳入 NXBIN (NX 11+ 必需)、ugii 与 STEP214UG
                QString nxBinDir = detectedUgBaseDir + "/NXBIN";
                QString ugiiDir = detectedUgBaseDir + "/ugii";
                QString step214Dir = detectedUgBaseDir + "/STEP214UG";
                QString extraPaths;
                if (QDir(nxBinDir).exists()) extraPaths += QDir::toNativeSeparators(nxBinDir) + ";";
                if (QDir(ugiiDir).exists()) extraPaths += QDir::toNativeSeparators(ugiiDir) + ";";
                if (QDir(step214Dir).exists()) extraPaths += QDir::toNativeSeparators(step214Dir) + ";";
                env.insert("PATH", extraPaths + env.value("PATH"));

                proc.setProcessEnvironment(env);
                proc.setWorkingDirectory(cacheDir);

                QString defFile = QDir::toNativeSeparators(detectedUgBaseDir + "/STEP214UG/ugstep214.def");
                QString nativeIn = QDir::toNativeSeparators(path);
                QString nativeTempOut = QDir::toNativeSeparators(tempStpPath);

                QStringList args;
                args << nativeIn;
                args << ("o=" + nativeTempOut);
                if (QFile::exists(defFile)) {
                    args << ("d=" + defFile);
                }

                traceWorkerLog("parsePRT: Launching command: " + translatorExe + " " + args.join(" "));

#ifdef Q_OS_WIN
                proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
                    args->flags |= CREATE_NO_WINDOW;
                });
#endif
                proc.start(translatorExe, args);

                // 加固: 轮询等待同时实时排空匿名管道缓冲，避免大量转码日志撑满管道导致子进程挂起死锁
                int elapsedMs = 0;
                const int timeoutMs = 60000;
                QByteArray stdOutBytes;
                QByteArray stdErrBytes;
                while (proc.state() != QProcess::NotRunning && elapsedMs < timeoutMs) {
                    if (m_cancelRequested.load()) {
                        traceWorkerLog("parsePRT: User cancelled conversion, killing translator process.");
                        proc.kill();
                        proc.waitForFinished(1000);
                        QFile::remove(tempStpPath);
                        return false;
                    }
                    proc.waitForFinished(200);
                    stdOutBytes.append(proc.readAllStandardOutput());
                    stdErrBytes.append(proc.readAllStandardError());
                    elapsedMs += 200;
                }

                if (proc.state() != QProcess::NotRunning) {
                    traceWorkerLog("parsePRT: Translator process timed out after 60s, terminating.");
                    proc.kill();
                    proc.waitForFinished(1000);
                    QFile::remove(tempStpPath);
                }

                stdOutBytes.append(proc.readAllStandardOutput());
                stdErrBytes.append(proc.readAllStandardError());

                QString stdOut = QString::fromLocal8Bit(stdOutBytes);
                QString stdErr = QString::fromLocal8Bit(stdErrBytes);
                traceWorkerLog(QString("parsePRT: Translator finished with exitCode: %1, exitStatus: %2")
                               .arg(proc.exitCode()).arg(proc.exitStatus()));
                if (!stdOut.isEmpty()) {
                    traceWorkerLog("parsePRT: step214ug stdOut:\n" + stdOut.trimmed());
                }
                if (!stdErr.isEmpty()) {
                    traceWorkerLog("parsePRT: step214ug stdErr:\n" + stdErr.trimmed());
                }

                // 校验转码产物与 STEP 文件格式标头（严格校验 ISO-10303-21 工业标准头）
                bool tempValid = false;
                if (proc.exitStatus() == QProcess::NormalExit && QFile::exists(tempStpPath) && QFileInfo(tempStpPath).size() > 0) {
                    QFile checkTemp(tempStpPath);
                    if (checkTemp.open(QIODevice::ReadOnly)) {
                        QByteArray stpHead = checkTemp.read(1024);
                        checkTemp.close();
                        if (stpHead.contains("ISO-10303-21")) {
                            tempValid = true;
                        }
                    }
                }

                if (tempValid) {
                    traceWorkerLog("parsePRT: Translation verified! Temp size: " + QString::number(QFileInfo(tempStpPath).size()));
#ifdef Q_OS_WIN
                    // 加固: Windows 原生 MoveFileExW 原子覆盖替换目标缓存文件
                    std::wstring wTemp = QDir::toNativeSeparators(tempStpPath).toStdWString();
                    std::wstring wDest = QDir::toNativeSeparators(cachedStpPath).toStdWString();
                    if (MoveFileExW(wTemp.c_str(), wDest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                        stpReady = true;
                    } else {
                        // 若改名失败且目标不存在，临时文件直接兜底作为本轮就绪模型
                        if (!QFile::exists(cachedStpPath)) {
                            cachedStpPath = tempStpPath;
                            stpReady = true;
                        } else {
                            QFile::remove(tempStpPath);
                        }
                    }
#else
                    if (QFile::exists(cachedStpPath)) QFile::remove(cachedStpPath);
                    if (QFile::rename(tempStpPath, cachedStpPath)) {
                        stpReady = true;
                    } else if (!QFile::exists(cachedStpPath)) {
                        cachedStpPath = tempStpPath;
                        stpReady = true;
                    } else {
                        QFile::remove(tempStpPath);
                    }
#endif
                } else {
                    QFile::remove(tempStpPath);
                    traceWorkerLog("parsePRT: Translation failed or output invalid!");
                }
            }

            if (stpReady) {
                emit sigProgress(50, "UG 模型转码完成，正在载入拓扑几何与零件装配树...");
                bool stepSuccess = parseSTEP(cachedStpPath, outModel);
                if (stepSuccess && outModel) {
                    // 保持原始 prt 文件路径与元数据呈现
                    outModel->filePath = path;
                    outModel->format = "UG/NX PRT";
                    traceWorkerLog("parsePRT: Successfully loaded UG model via headless bridge: " + path);
                    return true;
                } else {
                    // 若 parseSTEP 失败，主动清理损坏的缓存，避免后续持续复用坏文件
                    traceWorkerLog("parsePRT: parseSTEP failed on generated STEP file, purging cached file: " + cachedStpPath);
                    QFile::remove(cachedStpPath);
                }
            }
        }
    }

    // 降级引导卡片 (使用已探测到的 detectedUgBaseDir)
    bool hasLocalNX = (!detectedUgBaseDir.isEmpty() && QDir(detectedUgBaseDir).exists());

    // 构造专业的 CAD 专有格式引导提示 (降级方案)
    QString hintMsg;
    if (isSiemensNX) {
        if (hasLocalNX) {
            hintMsg = QString(
                "检测到【%1】专有零件格式 (.prt)\n"
                "本机已检测到 UG/NX 安装目录: %2\n\n"
                "💡 建议方案：\n"
                "1. 本地静默转码未能导出该模型（可能是装配引用缺失或许可证限制）\n"
                "2. 建议在 UG/NX 中手动导出为 STEP (.stp) 或 JT 格式，即可享受秒级无损 3D 预览与装配树交互"
            ).arg(cadVendor, detectedUgBaseDir);
        } else {
            hintMsg = QString(
                "检测到【%1】专有零件格式 (.prt)\n"
                "此格式为西门子私有闭源二进制容器 (Parasolid 内部数据流)\n\n"
                "💡 快速预览建议：\n"
                "请在设计端另存/导出为 STEP (.stp / .step) 或 IGES (.igs) 工业标准格式\n"
                "3dmaster 将为您提供真实材质色彩、截面剖切、装配树与特征棱线的完整极速预览"
            ).arg(cadVendor);
        }
    } else if (isCreo) {
        hintMsg = QString(
            "检测到【%1】专有零件格式 (.prt)\n"
            "此格式为 PTC 私有闭源参数化模型文件\n\n"
            "💡 快速预览建议：\n"
            "请在 Creo 中导出为 STEP (.stp) 工业中性格式，即可在 3dmaster 中完整呈现"
        ).arg(cadVendor);
    } else {
        hintMsg = "当前 .prt 文件为专有 CAD 封闭模型格式\n\n💡 建议在原 CAD 软件中导出为 STEP (.stp) 格式以在 3dmaster 中预览。";
    }

    m_customErrorMessage = hintMsg;
    return false;
}


