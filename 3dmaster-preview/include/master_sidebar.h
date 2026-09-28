#pragma once

#include "master_widget.h"
#include <QWidget>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QSlider>

/**
 * @brief 3dmaster 紧凑型折叠侧边控制面板
 */
class MasterSidebar : public QWidget {
    Q_OBJECT
public:
    explicit MasterSidebar(QWidget* parent = nullptr);
    virtual ~MasterSidebar() override = default;

    void updateModelStats(ModelDataPtr model);
    void setNavigationPreset(MasterWidget::NavigationPreset preset);
    MasterWidget::NavigationPreset navigationPreset() const;

signals:
    void sigCameraPreset(MasterWidget::CameraPreset preset);
    void sigNavigationPresetChanged(MasterWidget::NavigationPreset preset);
    void sigFitViewRequested();
    void sigShadingModeChanged(MasterWidget::ShadingMode mode);
    void sigShowGrid(bool show);
    void sigShowAxis(bool show);
    void sigShowWireframe(bool show);
    void sigShowFeatureEdges(bool show);
    void sigShowBoundingBox(bool show);
    void sigOrthographic(bool ortho);

    // 动态剖切控制
    void sigSectionEnabled(bool enabled);
    void sigSectionAxisChanged(int axis);
    void sigSectionDepthChanged(float depthPercent);

    // 零件装配体独立显隐控制
    void sigPartVisibleChanged(int partId, bool visible);
    void sigAllPartsVisibleChanged(bool visible);

private:
    void setupUI();

private:
    QComboBox* m_comboNavPreset = nullptr;
    QPushButton* m_btnFit = nullptr;
    QComboBox* m_comboShading = nullptr;
    QCheckBox* m_chkOrtho = nullptr;
    QCheckBox* m_chkGrid = nullptr;
    QCheckBox* m_chkAxis = nullptr;
    QCheckBox* m_chkWireframe = nullptr;
    QCheckBox* m_chkFeatureEdges = nullptr;
    QCheckBox* m_chkBoundingBox = nullptr;

    // 装配体多零件独立显隐
    class QGroupBox* m_grpAssembly = nullptr;
    class QTreeWidget* m_treeAssembly = nullptr;
    QPushButton* m_btnShowAll = nullptr;
    QPushButton* m_btnHideAll = nullptr;
    QPushButton* m_btnInvert = nullptr;

    // 动态剖切
    QCheckBox* m_chkSectionEnabled = nullptr;
    QComboBox* m_comboSectionAxis = nullptr;
    QSlider* m_sliderSectionDepth = nullptr;
    QLabel* m_lblSectionDepth = nullptr;

    QLabel* m_lblName = nullptr;
    QLabel* m_lblFormat = nullptr;
    QLabel* m_lblVerts = nullptr;
    QLabel* m_lblFaces = nullptr;
    QLabel* m_lblSize = nullptr;
};
