cmake_minimum_required(VERSION 3.20)
project(3dmaster_preview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Qt6 模块
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Network)

# 动态定位 OCCT 目录 (支持外置参数 -DOCCT_DIR=... 或自动查找兄弟目录)
if(NOT OCCT_DIR)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../occt")
        set(OCCT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../occt")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../3dmaster/occt")
        set(OCCT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../3dmaster/occt")
    endif()
endif()

# 引入头文件目录
include_directories(
    include
    "${OCCT_DIR}/inc"
)

# 源码清单：独立 Preview 外壳源码（3D 渲染核心在 3dmaster-core submodule）
set(SOURCES
    src/preview_main.cpp
    src/preview_window.cpp
    src/ipc_server.cpp
)

set(HEADERS
    include/preview_window.h
    include/ipc_server.h
)

# 共享核心 (3dmaster-core, git submodule at repo-root core/): 解析器/模型数据/GL 渲染/侧边栏/i18n 的唯一真相源
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../core" "${CMAKE_CURRENT_BINARY_DIR}/3dmaster_core")

add_executable(3dmaster-preview WIN32 ${SOURCES} ${HEADERS})

target_link_directories(3dmaster-preview PRIVATE "${OCCT_DIR}/win64/vc14/lib")

target_link_libraries(3dmaster-preview PRIVATE
    3dmaster_core
    Qt6::Core
    Qt6::CorePrivate
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Network
    TKDESTEP
    TKDEIGES
    TKDEGLTF
    TKDE
    TKRWMesh
    TKXCAF
    TKLCAF
    TKCAF
    TKService
    TKXSBase
    TKMesh
    TKBRep
    TKTopAlgo
    TKG3d
    TKG2d
    TKMath
    TKernel
    TKGeomBase
    TKGeomAlgo
    TKShHealing
    opengl32
    user32
)

target_compile_definitions(3dmaster-preview PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
