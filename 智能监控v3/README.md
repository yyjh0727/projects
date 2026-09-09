# 智能监控 v3 — 九宫格多路监控

> 团队项目（三人协作），本人负责 **UI 与交互**。本目录为 v3 迭代版本。

## 功能特性

- 九宫格 3×3 多路视频同屏显示，每路独立采集线程、独立运动检测
- 双击格子放大查看，Esc / 再次双击返回九宫格
- 画面叠加：CAM 名称、REC 录制标志、运动状态文字，边框四色区分状态（灰/绿/黄/红）
- 全局灵敏度滑块，实时调节 MOG2 运动检测阈值
- 右侧面板多路状态汇总：已连接路数 / 运动中 / 录制中 / 平均帧率
- 运动触发自动录制，静止 3 秒自动停止，录像按路号+时间戳命名

## 技术要点

- Qt Widgets 自定义控件（QPainter 六层绘制：背景/帧/名称/REC/状态/边框）
- Qt 信号槽队列连接实现采集子线程与 UI 线程的跨线程安全通信
- std::atomic 实现 UI → 线程的无锁反向控制（灵敏度实时生效）
- 控件 reparent + QStackedLayout 实现放大切换，视频流不断不重连

## 编译运行

```bash
sudo apt install build-essential cmake qt6-base-dev libopencv-dev
cd 智能监控v3
mkdir build && cd build
cmake .. && make -j$(nproc)
./SmartSurveillanceV3
```

视频源输入：`0` = 本机摄像头；视频文件路径；或 RTSP 地址。
同一文件路径点多次“添加并启动”可模拟多路。

已在 Ubuntu 24.04 + Qt 6.4.2 + OpenCV 4.6 编译运行验证：
3 路并发、运动检测画框、自动录制、帧率统计均正常。
