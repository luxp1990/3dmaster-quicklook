#include "master_sidebar.h"
#include <QGridLayout>
#include <QGroupBox>
#include <QFileInfo>
#include <QTreeWidget>
#include <QMenu>
#include <QAction>
#include <QPainter>
#include <QPixmap>
#include <QSignalBlocker>

MasterSidebar::MasterSidebar(QWidget* parent) : QWidget(parent) {
    setupUI();
}

void MasterSidebar::setupUI() {
    setFixedWidth(240);

    // 全局样式：应用于整个侧边栏（含 QScrollArea 内容）
    QString globalStyle = R"(
        QWidget {
            background-color: #111827;
            color: #E5E7EB;
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
            font-size: 12px;
        }
        QGroupBox {
            border: 1px solid #1F2937;
            border-radius: 6px;
            margin-top: 12px;
            padding-top: 14px;
            font-weight: bold;
            color: #38BDF8;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 4px;
        }
        QPushButton {
            background-color: #1F2937;
            border: 1px solid #374151;
            border-radius: 4px;
            color: #F3F4F6;
            padding: 5px 8px;
            min-height: 22px;
        }
        QPushButton:hover {
            background-color: #374151;
            border-color: #4B5563;
        }
        QPushButton:pressed {
            background-color: #0284C7;
            border-color: #0284C7;
        }
        QComboBox {
            background-color: #1F2937;
            border: 1px solid #374151;
            border-radius: 4px;
            color: #F3F4F6;
            padding: 4px 8px;
        }
        QCheckBox {
            spacing: 8px;
        }
        QCheckBox::indicator {
            width: 15px;
            height: 15px;
            border: 1px solid #4B5563;
            border-radius: 3px;
            background: #1F2937;
        }
        QCheckBox::indicator:checked {
            background: #0284C7;
            border-color: #38BDF8;
        }
        QScrollArea {
            background-color: #111827;
            border: none;
        }
        QScrollBar:vertical {
            background: #111827;
            width: 8px;
            margin: 0;
        }
        QScrollBar::handle:vertical {
            background: #374151;
            min-height: 30px;
            border-radius: 4px;
        }
        QScrollBar::handle:vertical:hover {
            background: #4B5563;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0;
        }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
            background: none;
        }
    )";
    setStyleSheet(globalStyle);

    // 外层布局：仅包含 QScrollArea，零边距
    auto outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    // QScrollArea 包裹所有侧边栏内容，窗口过小时可滚动
    auto scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto contentWidget = new QWidget(scrollArea);
    auto mainLayout = new QVBoxLayout(contentWidget);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(12);

    scrollArea->setWidget(contentWidget);
    outerLayout->addWidget(scrollArea);

    // 1. 相机视角组
    auto grpCamera = new QGroupBox("相机视角", this);
    auto camLayout = new QGridLayout(grpCamera);
    camLayout->setContentsMargins(8, 12, 8, 8);
    camLayout->setSpacing(6);

    auto btnFront = new QPushButton("前视", grpCamera);
    auto btnBack = new QPushButton("后视", grpCamera);
    auto btnLeft = new QPushButton("左视", grpCamera);
    auto btnRight = new QPushButton("右视", grpCamera);
    auto btnTop = new QPushButton("俯视", grpCamera);
    auto btnBtm = new QPushButton("仰视", grpCamera);
    m_btnFit = new QPushButton("适应窗口 (Ctrl+F)", grpCamera);
    auto btnReset = new QPushButton("重置视角 (R)", grpCamera);

    camLayout->addWidget(btnFront, 0, 0);
    camLayout->addWidget(btnBack, 0, 1);
    camLayout->addWidget(btnLeft, 1, 0);
    camLayout->addWidget(btnRight, 1, 1);
    camLayout->addWidget(btnTop, 2, 0);
    camLayout->addWidget(btnBtm, 2, 1);
    camLayout->addWidget(m_btnFit, 3, 0);
    camLayout->addWidget(btnReset, 3, 1);

    connect(btnFront, &QPushButton::clicked, this, [this]() { emit sigCameraPreset(MasterWidget::CameraPreset::Front); });
    connect(btnBack, &QPushButton::clicked, this, [this]() { emit sigCameraPreset(MasterWidget::CameraPreset::Back); });
    connect(btnLeft, &QPushButton::clicked, this, [this]() { emit sigCameraPreset(MasterWidget::CameraPreset::Left); });
    connect(btnRight, &QPushButton::clicked, this, [this]() { emit sigCameraPreset(MasterWidget::CameraPreset::Right); });
    connect(btnTop, &QPushButton::clicked, this, [this]() { emit sigCameraPreset(MasterWidget::CameraPreset::Top); });
    connect(btnBtm, &QPushButton::clicked, this, [this]() { emit sigCameraPreset(MasterWidget::CameraPreset::Bottom); });
    connect(btnReset, &QPushButton::clicked, this, [this]() { emit sigCameraPreset(MasterWidget::CameraPreset::Reset); });
    connect(m_btnFit, &QPushButton::clicked, this, [this]() { emit sigFitViewRequested(); });

    mainLayout->addWidget(grpCamera);

    // 2. 交互操作手感模式 (UG / SolidWorks / 通用)
    auto grpNav = new QGroupBox("交互操作手感", this);
    auto navLayout = new QVBoxLayout(grpNav);
    navLayout->setContentsMargins(8, 12, 8, 8);
    navLayout->setSpacing(6);

    m_comboNavPreset = new QComboBox(grpNav);
    m_comboNavPreset->addItem("📐 西门子 UG (Siemens NX)", static_cast<int>(MasterWidget::NavigationPreset::UG_NX));
    m_comboNavPreset->addItem("🔧 SolidWorks", static_cast<int>(MasterWidget::NavigationPreset::SolidWorks));
    m_comboNavPreset->addItem("🖱️ 经典通用 (Generic)", static_cast<int>(MasterWidget::NavigationPreset::DefaultGeneric));
    m_comboNavPreset->setToolTip("选择 3D 视口交互控制手感习惯");

    auto lblNavTip = new QLabel("中键旋转 · 左键平移 · 滚轮光标对心", grpNav);
    lblNavTip->setStyleSheet("color: #9CA3AF; font-size: 11px;");
    lblNavTip->setWordWrap(true);

    connect(m_comboNavPreset, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, lblNavTip](int index) {
        Q_UNUSED(index);
        auto preset = static_cast<MasterWidget::NavigationPreset>(m_comboNavPreset->currentData().toInt());
        if (preset == MasterWidget::NavigationPreset::UG_NX) {
            lblNavTip->setText("中键旋转 · 左键平移 · 滚轮光标对心");
        } else if (preset == MasterWidget::NavigationPreset::SolidWorks) {
            lblNavTip->setText("中键旋转 · Ctrl+中键平移 · 滚轮缩放");
        } else {
            lblNavTip->setText("左键旋转 · 右键平移 · 滚轮缩放");
        }
        emit sigNavigationPresetChanged(preset);
    });

    navLayout->addWidget(m_comboNavPreset);
    navLayout->addWidget(lblNavTip);
    mainLayout->addWidget(grpNav);

    // 2. 材质与色彩渲染组
    auto grpShading = new QGroupBox("材质与色彩渲染", this);
    auto shadeLayout = new QVBoxLayout(grpShading);
    shadeLayout->setContentsMargins(8, 12, 8, 8);
    shadeLayout->setSpacing(8);

    m_comboShading = new QComboBox(grpShading);
    m_comboShading->addItem("🎨 固有材质色彩 (Material)", 0);
    m_comboShading->addItem("🌈 顶点颜色 (Vertex Color)", 1);
    m_comboShading->addItem("⚪ 工业白模 (Pure White)", 2);
    m_comboShading->addItem("🧭 几何法线 (Normals)", 3);

    connect(m_comboShading, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        emit sigShadingModeChanged(static_cast<MasterWidget::ShadingMode>(index));
    });
    shadeLayout->addWidget(m_comboShading);

    m_chkOrtho = new QCheckBox("正交投影模式", grpShading);
    connect(m_chkOrtho, &QCheckBox::toggled, this, &MasterSidebar::sigOrthographic);
    shadeLayout->addWidget(m_chkOrtho);

    mainLayout->addWidget(grpShading);

    // 3. 辅助显示元素组
    auto grpDisplay = new QGroupBox("辅助显示", this);
    auto dispLayout = new QVBoxLayout(grpDisplay);
    dispLayout->setContentsMargins(8, 12, 8, 8);
    dispLayout->setSpacing(8);

    m_chkGrid = new QCheckBox("地面网格标尺", grpDisplay);
    m_chkGrid->setChecked(true);
    connect(m_chkGrid, &QCheckBox::toggled, this, &MasterSidebar::sigShowGrid);
    dispLayout->addWidget(m_chkGrid);

    m_chkAxis = new QCheckBox("3D 空间坐标轴", grpDisplay);
    m_chkAxis->setChecked(true);
    connect(m_chkAxis, &QCheckBox::toggled, this, &MasterSidebar::sigShowAxis);
    dispLayout->addWidget(m_chkAxis);

    m_chkWireframe = new QCheckBox("网格三角线框", grpDisplay);
    connect(m_chkWireframe, &QCheckBox::toggled, this, &MasterSidebar::sigShowWireframe);
    dispLayout->addWidget(m_chkWireframe);

    m_chkFeatureEdges = new QCheckBox("特征棱线 (CAD Edges)", grpDisplay);
    m_chkFeatureEdges->setChecked(true);
    connect(m_chkFeatureEdges, &QCheckBox::toggled, this, &MasterSidebar::sigShowFeatureEdges);
    dispLayout->addWidget(m_chkFeatureEdges);

    m_chkBoundingBox = new QCheckBox("三维尺寸包围盒", grpDisplay);
    m_chkBoundingBox->setChecked(false);
    connect(m_chkBoundingBox, &QCheckBox::toggled, this, &MasterSidebar::sigShowBoundingBox);
    dispLayout->addWidget(m_chkBoundingBox);

    mainLayout->addWidget(grpDisplay);

    // 4. 动态剖视控制组 (Section View)
    auto grpSection = new QGroupBox("动态剖视 (Section View)", this);
    auto secLayout = new QVBoxLayout(grpSection);
    secLayout->setContentsMargins(8, 12, 8, 8);
    secLayout->setSpacing(6);

    m_chkSectionEnabled = new QCheckBox("启用截面剖切", grpSection);
    connect(m_chkSectionEnabled, &QCheckBox::toggled, this, &MasterSidebar::sigSectionEnabled);
    secLayout->addWidget(m_chkSectionEnabled);

    m_comboSectionAxis = new QComboBox(grpSection);
    m_comboSectionAxis->addItem("📐 垂直 Y 轴 (水平剖切)", 1);
    m_comboSectionAxis->addItem("📐 垂直 X 轴 (左右剖切)", 0);
    m_comboSectionAxis->addItem("📐 垂直 Z 轴 (前后剖切)", 2);
    connect(m_comboSectionAxis, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        Q_UNUSED(index);
        emit sigSectionAxisChanged(m_comboSectionAxis->currentData().toInt());
    });
    secLayout->addWidget(m_comboSectionAxis);

    auto depthHeader = new QHBoxLayout();
    auto lblDepthTitle = new QLabel("剖切深度:", grpSection);
    lblDepthTitle->setStyleSheet("color: #9CA3AF; font-size: 11px;");
    m_lblSectionDepth = new QLabel("50%", grpSection);
    m_lblSectionDepth->setStyleSheet("color: #38BDF8; font-size: 11px; font-weight: bold;");
    depthHeader->addWidget(lblDepthTitle);
    depthHeader->addStretch();
    depthHeader->addWidget(m_lblSectionDepth);
    secLayout->addLayout(depthHeader);

    m_sliderSectionDepth = new QSlider(Qt::Horizontal, grpSection);
    m_sliderSectionDepth->setRange(0, 100);
    m_sliderSectionDepth->setValue(50);
    m_sliderSectionDepth->setStyleSheet(R"(
        QSlider::groove:horizontal {
            border: 1px solid #374151;
            height: 4px;
            background: #1F2937;
            border-radius: 2px;
        }
        QSlider::sub-page:horizontal {
            background: #0284C7;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            background: #38BDF8;
            border: 1px solid #0284C7;
            width: 12px;
            margin-top: -4px;
            margin-bottom: -4px;
            border-radius: 6px;
        }
    )");
    connect(m_sliderSectionDepth, &QSlider::valueChanged, this, [this](int val) {
        m_lblSectionDepth->setText(QString("%1%").arg(val));
        emit sigSectionDepthChanged(val / 100.0f);
    });
    secLayout->addWidget(m_sliderSectionDepth);

    mainLayout->addWidget(grpSection);

    // 5. 装配体组件列表组 (Assembly Components)
    m_grpAssembly = new QGroupBox("装配体组件清单", this);
    auto assemLayout = new QVBoxLayout(m_grpAssembly);
    assemLayout->setContentsMargins(6, 12, 6, 6);
    assemLayout->setSpacing(6);

    auto btnBar = new QHBoxLayout();
    btnBar->setSpacing(4);
    m_btnShowAll = new QPushButton("全显", m_grpAssembly);
    m_btnHideAll = new QPushButton("全隐", m_grpAssembly);
    m_btnInvert = new QPushButton("反选", m_grpAssembly);

    m_btnShowAll->setStyleSheet("font-size: 11px; padding: 2px 4px; min-height: 20px;");
    m_btnHideAll->setStyleSheet("font-size: 11px; padding: 2px 4px; min-height: 20px;");
    m_btnInvert->setStyleSheet("font-size: 11px; padding: 2px 4px; min-height: 20px;");

    btnBar->addWidget(m_btnShowAll);
    btnBar->addWidget(m_btnHideAll);
    btnBar->addWidget(m_btnInvert);
    assemLayout->addLayout(btnBar);

    m_treeAssembly = new QTreeWidget(m_grpAssembly);
    m_treeAssembly->setHeaderHidden(true);
    m_treeAssembly->setRootIsDecorated(false);
    m_treeAssembly->setContextMenuPolicy(Qt::CustomContextMenu);
    m_treeAssembly->setStyleSheet(R"(
        QTreeWidget {
            background-color: #0F172A;
            border: 1px solid #1E293B;
            border-radius: 4px;
            color: #E2E8F0;
            font-size: 11px;
            padding: 2px;
        }
        QTreeWidget::item {
            padding: 3px 2px;
            border-bottom: 1px solid #1E293B;
        }
        QTreeWidget::item:hover {
            background-color: #1E293B;
        }
        QTreeWidget::item:selected {
            background-color: #0284C7;
            color: #FFFFFF;
        }
        QTreeWidget::indicator {
            width: 14px;
            height: 14px;
        }
    )");
    m_treeAssembly->setMinimumHeight(130);
    m_treeAssembly->setMaximumHeight(200);
    assemLayout->addWidget(m_treeAssembly);

    connect(m_treeAssembly, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (column == 0 && item) {
            int partId = item->data(0, Qt::UserRole).toInt();
            bool checked = (item->checkState(0) == Qt::Checked);
            emit sigPartVisibleChanged(partId, checked);
        }
    });

    connect(m_btnShowAll, &QPushButton::clicked, this, [this]() {
        QSignalBlocker blocker(m_treeAssembly);
        for (int i = 0; i < m_treeAssembly->topLevelItemCount(); ++i) {
            auto* item = m_treeAssembly->topLevelItem(i);
            item->setCheckState(0, Qt::Checked);
        }
        emit sigAllPartsVisibleChanged(true);
    });

    connect(m_btnHideAll, &QPushButton::clicked, this, [this]() {
        QSignalBlocker blocker(m_treeAssembly);
        for (int i = 0; i < m_treeAssembly->topLevelItemCount(); ++i) {
            auto* item = m_treeAssembly->topLevelItem(i);
            item->setCheckState(0, Qt::Unchecked);
        }
        emit sigAllPartsVisibleChanged(false);
    });

    connect(m_btnInvert, &QPushButton::clicked, this, [this]() {
        for (int i = 0; i < m_treeAssembly->topLevelItemCount(); ++i) {
            auto* item = m_treeAssembly->topLevelItem(i);
            bool wasChecked = (item->checkState(0) == Qt::Checked);
            item->setCheckState(0, wasChecked ? Qt::Unchecked : Qt::Checked);
        }
    });

    connect(m_treeAssembly, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        auto* curItem = m_treeAssembly->itemAt(pos);
        QMenu menu(this);
        menu.setStyleSheet(R"(
            QMenu {
                background-color: #1E293B;
                border: 1px solid #334155;
                color: #F8FAFC;
                padding: 4px;
            }
            QMenu::item {
                padding: 5px 20px 5px 24px;
                border-radius: 3px;
            }
            QMenu::item:selected {
                background-color: #0284C7;
            }
        )");

        if (curItem) {
            int partId = curItem->data(0, Qt::UserRole).toInt();
            auto* actIsolate = menu.addAction("👁 仅显示此零件 (隔离模式)");
            connect(actIsolate, &QAction::triggered, this, [this, partId, curItem]() {
                QSignalBlocker blocker(m_treeAssembly);
                for (int i = 0; i < m_treeAssembly->topLevelItemCount(); ++i) {
                    auto* it = m_treeAssembly->topLevelItem(i);
                    bool target = (it == curItem);
                    it->setCheckState(0, target ? Qt::Checked : Qt::Unchecked);
                    emit sigPartVisibleChanged(it->data(0, Qt::UserRole).toInt(), target);
                }
            });
            menu.addSeparator();
        }

        auto* actAllShow = menu.addAction("✔ 全部显示");
        connect(actAllShow, &QAction::triggered, m_btnShowAll, &QPushButton::click);

        auto* actAllHide = menu.addAction("✖ 全部隐藏");
        connect(actAllHide, &QAction::triggered, m_btnHideAll, &QPushButton::click);

        auto* actInvert = menu.addAction("🔄 反选");
        connect(actInvert, &QAction::triggered, m_btnInvert, &QPushButton::click);

        menu.exec(m_treeAssembly->viewport()->mapToGlobal(pos));
    });

    m_grpAssembly->setVisible(false);
    mainLayout->addWidget(m_grpAssembly);

    // 6. 模型信息面板
    auto grpStats = new QGroupBox("模型资产信息", this);
    auto statsLayout = new QVBoxLayout(grpStats);
    statsLayout->setContentsMargins(8, 12, 8, 8);
    statsLayout->setSpacing(4);

    m_lblName = new QLabel("名称: -", grpStats);
    m_lblFormat = new QLabel("格式: -", grpStats);
    m_lblVerts = new QLabel("顶点数: -", grpStats);
    m_lblFaces = new QLabel("面片数: -", grpStats);
    m_lblSize = new QLabel("包围盒: -", grpStats);

    m_lblName->setStyleSheet("color: #38BDF8; font-weight: bold;");
    m_lblName->setWordWrap(true);
    m_lblFormat->setStyleSheet("color: #9CA3AF;");
    m_lblVerts->setStyleSheet("color: #9CA3AF;");
    m_lblFaces->setStyleSheet("color: #9CA3AF;");
    m_lblSize->setStyleSheet("color: #9CA3AF;");

    statsLayout->addWidget(m_lblName);
    statsLayout->addWidget(m_lblFormat);
    statsLayout->addWidget(m_lblVerts);
    statsLayout->addWidget(m_lblFaces);
    statsLayout->addWidget(m_lblSize);

    mainLayout->addWidget(grpStats);
    mainLayout->addStretch();
}

void MasterSidebar::updateModelStats(ModelDataPtr model) {
    if (!model) return;

    QString fileName = QFileInfo(model->filePath).fileName();
    m_lblName->setText(QString("名称: %1").arg(fileName));
    m_lblName->setToolTip(model->filePath);

    m_lblFormat->setText(QString("格式: %1").arg(model->format.toUpper()));
    m_lblVerts->setText(QString("顶点数: %1").arg(QLocale().toString(static_cast<qulonglong>(model->totalVertices))));
    m_lblFaces->setText(QString("面片数: %1").arg(QLocale().toString(static_cast<qulonglong>(model->totalTriangles))));

    QVector3D sz = model->boundsMax - model->boundsMin;
    m_lblSize->setText(QString("尺寸: %1 × %2 × %3")
                           .arg(sz.x(), 0, 'f', 1)
                           .arg(sz.y(), 0, 'f', 1)
                           .arg(sz.z(), 0, 'f', 1));

    if (model->partNames.size() > 1) {
        m_grpAssembly->setVisible(true);
        m_grpAssembly->setTitle(QString("装配体组件 (%1)").arg(model->partNames.size()));

        QSignalBlocker blocker(m_treeAssembly);
        m_treeAssembly->clear();

        for (auto it = model->partNames.begin(); it != model->partNames.end(); ++it) {
            int partId = it.key();
            QString name = it.value();
            uint64_t tris = model->partTriangles.value(partId, 0);

            auto* item = new QTreeWidgetItem(m_treeAssembly);
            item->setData(0, Qt::UserRole, partId);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            item->setCheckState(0, Qt::Checked);

            // 绘制彩色圆角矩形图标作为零件颜色标识
            QVector3D pCol = model->partColors.value(partId, QVector3D(0.72f, 0.76f, 0.82f));
            QPixmap pix(14, 14);
            pix.fill(Qt::transparent);
            QPainter p(&pix);
            p.setRenderHint(QPainter::Antialiasing);
            p.setBrush(QColor::fromRgbF(pCol.x(), pCol.y(), pCol.z()));
            p.setPen(QPen(QColor(255, 255, 255, 80), 1));
            p.drawRoundedRect(1, 1, 12, 12, 2, 2);
            p.end();

            item->setIcon(0, QIcon(pix));
            item->setText(0, QString("%1 (%2面)").arg(name).arg(tris));
            item->setToolTip(0, QString("零件 ID: %1\n名称: %2\n三角面数: %3\n右键可进行隔离显示").arg(partId).arg(name).arg(tris));
        }
    } else {
        m_grpAssembly->setVisible(false);
    }
}

void MasterSidebar::setNavigationPreset(MasterWidget::NavigationPreset preset) {
    if (!m_comboNavPreset) return;
    int idx = m_comboNavPreset->findData(static_cast<int>(preset));
    if (idx >= 0) {
        m_comboNavPreset->setCurrentIndex(idx);
    }
}

MasterWidget::NavigationPreset MasterSidebar::navigationPreset() const {
    if (!m_comboNavPreset) return MasterWidget::NavigationPreset::UG_NX;
    return static_cast<MasterWidget::NavigationPreset>(m_comboNavPreset->currentData().toInt());
}
