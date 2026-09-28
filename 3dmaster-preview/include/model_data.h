#pragma once

#include <QString>
#include <QVector>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <QMatrix4x4>
#include <QImage>
#include <QMap>
#include <memory>
#include <algorithm>
#include <optional>

/**
 * @brief 单个顶点的数据布局 (位置 + 法线 + 纹理坐标 + 顶点色)
 */
struct Vertex {
    QVector3D position;
    QVector3D normal;
    QVector2D texCoord;
    QVector4D color; // RGBA 顶点颜色 (默认 1.0f, 1.0f, 1.0f, 1.0f)

    Vertex() : color(1.0f, 1.0f, 1.0f, 1.0f) {}
    Vertex(const QVector3D& pos, const QVector3D& norm = QVector3D(), 
           const QVector2D& tex = QVector2D(), const QVector4D& col = QVector4D(1.0f, 1.0f, 1.0f, 1.0f))
        : position(pos), normal(norm), texCoord(tex), color(col) {}
};

/**
 * @brief 单个子网格 (Submesh / Part)
 */
struct SubMesh {
    QString name;
    QVector<Vertex> vertices;
    QVector<uint32_t> indices;

    // 材质与几何图元属性
    QVector3D diffuseColor = QVector3D(0.75f, 0.78f, 0.82f); // 固有材质底色
    float opacity = 1.0f;
    bool hasVertexColors = false;
    bool hasTexture = false;
    QImage textureImage; // 漫反射纹理图像
    uint32_t primitiveType = 0x0004; // 默认 GL_TRIANGLES (0x0004), 也可为 GL_LINES (0x0001) 或 GL_POINTS (0x0000)
    float lineWidth = 1.5f;
    bool isFeatureEdge = false; // 是否为 CAD 原生特征硬轮廓边 (GL_LINES)
    bool visible = true;        // 零件独立显隐状态
    int partId = 0;             // 归属的零件装配节点 ID

    // 原型实例化 (Prototype Instancing) 支持
    QMatrix4x4 transform;       // 实例世界空间变换矩阵 (默认单位阵)
    int prototypeId = -1;       // 引用的原型网格 ID (>= 0 时启用 GPU 共享缓冲)
    // 空间包围盒 (用于快速几何定位与8角点视锥体裁剪)
    QVector3D localAabbMin = QVector3D(1e9f, 1e9f, 1e9f);
    QVector3D localAabbMax = QVector3D(-1e9f, -1e9f, -1e9f);
    bool hasLocalAabb = false;

    void updateLocalAabb() {
        if (vertices.isEmpty()) return;
        localAabbMin = QVector3D(1e9f, 1e9f, 1e9f);
        localAabbMax = QVector3D(-1e9f, -1e9f, -1e9f);
        for (const auto& v : vertices) {
            localAabbMin.setX(std::min(localAabbMin.x(), v.position.x()));
            localAabbMin.setY(std::min(localAabbMin.y(), v.position.y()));
            localAabbMin.setZ(std::min(localAabbMin.z(), v.position.z()));
            localAabbMax.setX(std::max(localAabbMax.x(), v.position.x()));
            localAabbMax.setY(std::max(localAabbMax.y(), v.position.y()));
            localAabbMax.setZ(std::max(localAabbMax.z(), v.position.z()));
        }
        hasLocalAabb = true;
    }
};

/**
 * @brief 整个三维模型的内存聚合数据包
 */
struct ModelData {
    QString filePath;
    QString format;
    QVector<SubMesh> meshes;
    QMap<int, QString> partNames; // 零件 ID -> 零件名称映射表
    QMap<int, QVector3D> partColors; // 零件 ID -> 零件材质主色表
    QMap<int, uint64_t> partTriangles; // 零件 ID -> 零件面数统计表

    // 空间包围盒
    QVector3D boundsMin;
    QVector3D boundsMax;
    QVector3D center;
    float boundingRadius = 1.0f;

    // 统计指标
    uint64_t totalVertices = 0;
    uint64_t totalTriangles = 0;
    bool hasMaterials = false;
    bool hasTextures = false;

    void calculateBounds() {
        totalVertices = 0;
        totalTriangles = 0;
        hasMaterials = false;
        hasTextures = false;
        partTriangles.clear();

        boundsMin = QVector3D(1e9f, 1e9f, 1e9f);
        boundsMax = QVector3D(-1e9f, -1e9f, -1e9f);

        for (auto& mesh : meshes) {
            if (!mesh.isFeatureEdge) {
                if (!mesh.name.isEmpty() && !partNames.contains(mesh.partId)) {
                    partNames[mesh.partId] = mesh.name;
                }
                if (!partColors.contains(mesh.partId)) {
                    partColors[mesh.partId] = mesh.diffuseColor;
                }
                if (mesh.primitiveType == 0x0004) {
                    uint64_t tris = mesh.indices.size() / 3;
                    totalTriangles += tris;
                    partTriangles[mesh.partId] += tris;
                }
            }
            totalVertices += mesh.vertices.size();
            if (mesh.hasTexture) hasTextures = true;
            if (mesh.hasVertexColors) hasMaterials = true;
            if (mesh.diffuseColor != QVector3D(0.75f, 0.78f, 0.82f)) hasMaterials = true;

            if (!mesh.hasLocalAabb) {
                mesh.updateLocalAabb();
            }

            if (mesh.hasLocalAabb) {
                const bool hasTrsf = !mesh.transform.isIdentity();
                if (!hasTrsf) {
                    boundsMin.setX(std::min(boundsMin.x(), mesh.localAabbMin.x()));
                    boundsMin.setY(std::min(boundsMin.y(), mesh.localAabbMin.y()));
                    boundsMin.setZ(std::min(boundsMin.z(), mesh.localAabbMin.z()));
                    boundsMax.setX(std::max(boundsMax.x(), mesh.localAabbMax.x()));
                    boundsMax.setY(std::max(boundsMax.y(), mesh.localAabbMax.y()));
                    boundsMax.setZ(std::max(boundsMax.z(), mesh.localAabbMax.z()));
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
                    for (int i = 0; i < 8; ++i) {
                        boundsMin.setX(std::min(boundsMin.x(), corners[i].x()));
                        boundsMin.setY(std::min(boundsMin.y(), corners[i].y()));
                        boundsMin.setZ(std::min(boundsMin.z(), corners[i].z()));
                        boundsMax.setX(std::max(boundsMax.x(), corners[i].x()));
                        boundsMax.setY(std::max(boundsMax.y(), corners[i].y()));
                        boundsMax.setZ(std::max(boundsMax.z(), corners[i].z()));
                    }
                }
            }
        }

        if (totalVertices == 0) {
            boundsMin = QVector3D(-1, -1, -1);
            boundsMax = QVector3D(1, 1, 1);
            center = QVector3D(0, 0, 0);
            boundingRadius = 1.0f;
            return;
        }

        center = (boundsMin + boundsMax) * 0.5f;
        boundingRadius = (boundsMax - boundsMin).length() * 0.5f;
        if (boundingRadius < 1e-4f) boundingRadius = 1.0f;
    }
};

#include <QMetaType>
using ModelDataPtr = std::shared_ptr<ModelData>;
Q_DECLARE_METATYPE(ModelDataPtr)
