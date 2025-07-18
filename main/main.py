import sys
import socket
import threading
import json
from datetime import datetime
import re
import time
import csv
import os
from PyQt5.QtWidgets import (
    QApplication, QWidget, QVBoxLayout, QPushButton,
    QTextEdit, QLineEdit, QLabel, QHBoxLayout,
    QGroupBox, QGridLayout, QComboBox, QTabWidget,
    QSplitter, QTableWidget, QTableWidgetItem, QHeaderView,
    QStatusBar, QFileDialog, QMessageBox
)
from PyQt5.QtCore import pyqtSignal, QObject, Qt, QTimer
from PyQt5.QtGui import QColor
import websocket
import pyqtgraph as pg


class Worker(QObject):
    message_received = pyqtSignal(str)
    data_received = pyqtSignal(dict)  # 新增：接收解析后的数据
    connection_status = pyqtSignal(str)


class WebSocketClient:
    def __init__(self, uri, worker: Worker):
        self.uri = uri
        self.worker = worker
        self.ws = None
        self.is_connected = False
        self.should_reconnect = True
        self.thread = threading.Thread(target=self.run)
        self.thread.daemon = True
        self.thread.start()

    def run(self):
        self.ws = websocket.WebSocketApp(
            self.uri,
            on_open=self.on_open,
            on_message=self.on_message,
            on_error=self.on_error,
            on_close=self.on_close
        )
        self.ws.run_forever()

    def on_open(self, _ws):
        self.is_connected = True
        self.worker.connection_status.emit("✅ WebSocket 连接成功")
        # self.send("START")

    def on_message(self, _ws, message):
        # 检查并处理二进制数据
        if isinstance(message, bytes):
            try:
                # 尝试将二进制数据解码为UTF-8文本
                decoded_message = message.decode('utf-8')
                # 清理显示，移除多余的换行符
                clean_message = decoded_message.strip()
                self.worker.message_received.emit(f"📥 接收: {clean_message}")
                message = clean_message  # 使用清理后的文本进行后续处理
            except UnicodeDecodeError:
                # 如果解码失败，显示原始二进制数据的十六进制表示
                hex_preview = ' '.join([f"{b:02x}" for b in message[:20]])
                self.worker.message_received.emit(f"📥 接收二进制数据: {len(message)}字节 - 前20字节: {hex_preview}...")
                return  # 无法解码的二进制数据不进行后续处理
        else:
            # 已经是文本格式的数据
            clean_message = message.strip() if isinstance(message, str) else str(message)
            self.worker.message_received.emit(f"📥 接收: {clean_message}")
            message = clean_message
            
        # 尝试解析数据
        try:
            # 检查是否为数据记录格式，允许更灵活的前缀匹配（包括中文）
            match = re.search(r'([\w\u4e00-\u9fff]+)\s+T=(\d+),SPD=(-?\d+),(-?\d+),(-?\d+),(-?\d+),ACC=(-?\d+),(-?\d+),(-?\d+),GYRO=(-?\d+),(-?\d+),(-?\d+),S=(\d+)', message)
            if match:
                prefix = match.group(1)
                data = {
                    "prefix": prefix,
                    "timestamp": int(match.group(2)),
                    "speed": [int(match.group(3)), int(match.group(4)), int(match.group(5)), int(match.group(6))],
                    "accel": [int(match.group(7)), int(match.group(8)), int(match.group(9))],
                    "gyro": [int(match.group(10)), int(match.group(11)), int(match.group(12))],
                    "status": int(match.group(13))
                }
                self.worker.data_received.emit(data)
            else:
                # 对于不匹配数据记录格式的消息，尝试解析为JSON
                try:
                    if message.startswith('{') and message.endswith('}'):
                        json_data = json.loads(message)
                        print(f"解析JSON成功: {json_data}")
                except:
                    pass  # 不是JSON格式，忽略
        except Exception as e:
            print(f"解析数据错误: {e}")

    def on_error(self, _ws, error):
        self.is_connected = False
        self.worker.connection_status.emit(f"❌ 错误: {error}")

    def on_close(self, _ws, _close_status_code, _close_msg):
        self.is_connected = False
        self.worker.connection_status.emit("🔌 连接关闭")

    def close(self):
        """主动关闭连接"""
        self.should_reconnect = False
        if self.ws:
            self.ws.close()

    def send(self, msg):
        if self.ws:
            try:
                self.ws.send(msg)
                self.worker.message_received.emit(f"📤 发送: {msg}")
            except Exception as e:
                self.worker.message_received.emit(f"❌ 发送失败: {e}")


class DataGraphs(QWidget):
    def __init__(self):
        super().__init__()
        layout = QVBoxLayout(self)
        
        # 创建绘图窗口
        self.speed_plot = pg.PlotWidget(title="轮速")
        self.speed_plot.addLegend()
        self.speed_plot.setLabel('left', '速度')
        self.speed_plot.setLabel('bottom', '时间 (s)')
        
        self.accel_plot = pg.PlotWidget(title="加速度")
        self.accel_plot.addLegend()
        self.accel_plot.setLabel('left', '加速度')
        self.accel_plot.setLabel('bottom', '时间 (s)')
        
        self.gyro_plot = pg.PlotWidget(title="角速度")
        self.gyro_plot.addLegend()
        self.gyro_plot.setLabel('left', '角速度')
        self.gyro_plot.setLabel('bottom', '时间 (s)')
        
        layout.addWidget(self.speed_plot)
        layout.addWidget(self.accel_plot)
        layout.addWidget(self.gyro_plot)
        
        # 初始化数据
        self.timestamps = []
        self.speed_data = [[], [], [], []]
        self.accel_data = [[], [], []]
        self.gyro_data = [[], [], []]
        
        # 创建曲线
        self.speed_curves = []
        for i in range(4):
            curve = pg.PlotDataItem(pen=(i, 4), name=f"轮{i+1}")
            self.speed_plot.addItem(curve)
            self.speed_curves.append(curve)
        
        self.accel_curves = []
        for i, axis in enumerate(['X', 'Y', 'Z']):
            curve = pg.PlotDataItem(pen=(i, 3), name=f"加速度{axis}")
            self.accel_plot.addItem(curve)
            self.accel_curves.append(curve)
        
        self.gyro_curves = []
        for i, axis in enumerate(['X', 'Y', 'Z']):
            curve = pg.PlotDataItem(pen=(i, 3), name=f"角速度{axis}")
            self.gyro_plot.addItem(curve)
            self.gyro_curves.append(curve)
        
        # 设置数据长度限制
        self.max_data_length = 500

        # 性能优化：预分配数据缓冲区
        self._temp_timestamps = []
        self._temp_speed_data = [[] for _ in range(4)]
        self._temp_accel_data = [[] for _ in range(3)]
        self._temp_gyro_data = [[] for _ in range(3)]
    
    def update_data(self, data):
        # 转换时间戳为相对时间(秒)
        if not self.timestamps:
            self.start_time = data["timestamp"]
        rel_time = (data["timestamp"] - self.start_time) / 1000.0
        
        # 添加新数据点
        self.timestamps.append(rel_time)
        
        for i in range(4):
            self.speed_data[i].append(data["speed"][i])
        
        for i in range(3):
            self.accel_data[i].append(data["accel"][i])
            self.gyro_data[i].append(data["gyro"][i])
        
        # 限制数据长度
        if len(self.timestamps) > self.max_data_length:
            self.timestamps = self.timestamps[-self.max_data_length:]
            for i in range(4):
                self.speed_data[i] = self.speed_data[i][-self.max_data_length:]
            for i in range(3):
                self.accel_data[i] = self.accel_data[i][-self.max_data_length:]
                self.gyro_data[i] = self.gyro_data[i][-self.max_data_length:]
        
        # 更新图表
        for i in range(4):
            self.speed_curves[i].setData(self.timestamps, self.speed_data[i])
        
        for i in range(3):
            self.accel_curves[i].setData(self.timestamps, self.accel_data[i])
            self.gyro_curves[i].setData(self.timestamps, self.gyro_data[i])

    def clear_data(self):
        """清空所有图表数据"""
        self.timestamps.clear()
        for i in range(4):
            self.speed_data[i].clear()
        for i in range(3):
            self.accel_data[i].clear()
            self.gyro_data[i].clear()

        # 清空图表显示
        for curve in self.speed_curves:
            curve.clear()
        for curve in self.accel_curves:
            curve.clear()
        for curve in self.gyro_curves:
            curve.clear()

    def export_to_csv(self, filename):
        """导出图表数据到CSV文件"""
        try:
            with open(filename, 'w', newline='', encoding='utf-8') as csvfile:
                writer = csv.writer(csvfile)

                # 写入表头
                headers = ['时间戳(s)', '轮速1', '轮速2', '轮速3', '轮速4',
                          '加速度X', '加速度Y', '加速度Z',
                          '角速度X', '角速度Y', '角速度Z']
                writer.writerow(headers)

                # 写入数据
                for i in range(len(self.timestamps)):
                    row = [self.timestamps[i]]

                    # 添加轮速数据
                    for j in range(4):
                        if i < len(self.speed_data[j]):
                            row.append(self.speed_data[j][i])
                        else:
                            row.append('')

                    # 添加加速度数据
                    for j in range(3):
                        if i < len(self.accel_data[j]):
                            row.append(self.accel_data[j][i])
                        else:
                            row.append('')

                    # 添加角速度数据
                    for j in range(3):
                        if i < len(self.gyro_data[j]):
                            row.append(self.gyro_data[j][i])
                        else:
                            row.append('')

                    writer.writerow(row)

            return True, f"成功导出 {len(self.timestamps)} 条数据到 {filename}"

        except Exception as e:
            return False, f"导出失败: {str(e)}"


class DataTable(QTableWidget):
    def __init__(self):
        super().__init__()
        self.setColumnCount(13)
        self.setHorizontalHeaderLabels([
            "前缀", "时间戳", "轮速1", "轮速2", "轮速3", "轮速4", 
            "加速度X", "加速度Y", "加速度Z", 
            "角速度X", "角速度Y", "角速度Z", "状态"
        ])
        self.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch)
        self.setEditTriggers(QTableWidget.NoEditTriggers)
        
        # 最大行数限制
        self.max_rows = 100
    
    def add_data(self, data):
        row = self.rowCount()
        self.insertRow(row)
        
        self.setItem(row, 0, QTableWidgetItem(data["prefix"]))
        self.setItem(row, 1, QTableWidgetItem(str(data["timestamp"])))
        
        for i in range(4):
            self.setItem(row, 2+i, QTableWidgetItem(str(data["speed"][i])))
        
        for i in range(3):
            self.setItem(row, 6+i, QTableWidgetItem(str(data["accel"][i])))
            self.setItem(row, 9+i, QTableWidgetItem(str(data["gyro"][i])))
        
        self.setItem(row, 12, QTableWidgetItem(str(data["status"])))
        
        # 根据前缀设置颜色
        if "实时" in data["prefix"]:
            color = QColor(220, 255, 220)  # 淡绿色
        elif "历史" in data["prefix"]:
            color = QColor(220, 220, 255)  # 淡蓝色
        else:
            color = QColor(255, 255, 220)  # 淡黄色
        
        for col in range(self.columnCount()):
            self.item(row, col).setBackground(color)
        
        # 滚动到最新行
        self.scrollToBottom()
        
        # 限制最大行数
        if self.rowCount() > self.max_rows:
            self.removeRow(0)

    def clear_data(self):
        """清空表格数据"""
        self.setRowCount(0)

    def export_to_csv(self, filename):
        """导出表格数据到CSV文件"""
        try:
            with open(filename, 'w', newline='', encoding='utf-8') as csvfile:
                writer = csv.writer(csvfile)

                # 写入表头
                headers = []
                for col in range(self.columnCount()):
                    headers.append(self.horizontalHeaderItem(col).text())
                writer.writerow(headers)

                # 写入数据
                for row in range(self.rowCount()):
                    row_data = []
                    for col in range(self.columnCount()):
                        item = self.item(row, col)
                        if item:
                            row_data.append(item.text())
                        else:
                            row_data.append('')
                    writer.writerow(row_data)

            return True, f"成功导出 {self.rowCount()} 行数据到 {filename}"

        except Exception as e:
            return False, f"导出失败: {str(e)}"


class MainWindow(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("ABS控制系统上位机")
        self.resize(1000, 800)
        
        # 添加用户选项卡锁定标志
        self.tab_lock = False

        # 帧率计算相关变量
        self.frame_count = 0
        self.last_frame_time = time.time()
        self.frame_rate_timer = QTimer()
        self.frame_rate_timer.timeout.connect(self.update_frame_rate)
        self.frame_rate_timer.start(1000)  # 每秒更新一次帧率
        
        # 主布局
        main_layout = QVBoxLayout(self)
        
        # 顶部连接区域
        connection_group = QGroupBox("连接设置")
        connection_layout = QHBoxLayout()

        self.status_label = QLabel("未连接")
        self.connect_button = QPushButton("连接 ESP32")
        self.disconnect_button = QPushButton("断开连接")
        self.disconnect_button.setEnabled(False)  # 初始状态为禁用

        # 添加数据帧率显示
        self.frame_rate_label = QLabel("帧率: 0.0 fps")

        connection_layout.addWidget(self.status_label)
        connection_layout.addWidget(self.frame_rate_label)
        connection_layout.addWidget(self.connect_button)
        connection_layout.addWidget(self.disconnect_button)
        connection_group.setLayout(connection_layout)
        main_layout.addWidget(connection_group)
        
        # 模式控制区域
        mode_group = QGroupBox("工作模式")
        mode_layout = QHBoxLayout()
        
        self.mode_default_btn = QPushButton("默认模式")
        self.mode_realtime_btn = QPushButton("实时模式")
        self.mode_history_btn = QPushButton("回溯模式")
        
        mode_layout.addWidget(self.mode_default_btn)
        mode_layout.addWidget(self.mode_realtime_btn)
        mode_layout.addWidget(self.mode_history_btn)
        mode_group.setLayout(mode_layout)
        main_layout.addWidget(mode_group)
        
        # Flash操作区域
        flash_group = QGroupBox("Flash操作")
        flash_layout = QHBoxLayout()

        self.flash_info_btn = QPushButton("Flash信息")
        self.flash_read_btn = QPushButton("读取数据")
        self.flash_erase_btn = QPushButton("擦除数据")
        self.clear_data_btn = QPushButton("清空显示")
        self.export_data_btn = QPushButton("导出数据")

        self.read_index_input = QLineEdit()
        self.read_index_input.setPlaceholderText("索引")
        self.read_count_input = QLineEdit()
        self.read_count_input.setPlaceholderText("数量")

        flash_layout.addWidget(self.flash_info_btn)
        flash_layout.addWidget(self.flash_read_btn)
        flash_layout.addWidget(self.read_index_input)
        flash_layout.addWidget(self.read_count_input)
        flash_layout.addWidget(self.flash_erase_btn)
        flash_layout.addWidget(self.clear_data_btn)
        flash_layout.addWidget(self.export_data_btn)
        flash_group.setLayout(flash_layout)
        main_layout.addWidget(flash_group)
        
        # 命令输入区域
        cmd_group = QGroupBox("命令输入")
        cmd_layout = QHBoxLayout()
        
        self.send_input = QLineEdit()
        self.send_input.setPlaceholderText("输入要发送的命令")
        self.send_button = QPushButton("发送")
        
        cmd_layout.addWidget(self.send_input)
        cmd_layout.addWidget(self.send_button)
        cmd_group.setLayout(cmd_layout)
        main_layout.addWidget(cmd_group)
        
        # 数据显示区域 - 使用选项卡
        self.tab_widget = QTabWidget()
        
        # 选项卡1: 日志
        self.log_output = QTextEdit()
        self.log_output.setReadOnly(True)
        
        # 选项卡2: 数据图表
        self.data_graphs = DataGraphs()
        
        # 选项卡3: 数据表格
        self.data_table = DataTable()
        
        self.tab_widget.addTab(self.log_output, "日志")
        self.tab_widget.addTab(self.data_graphs, "图表")
        self.tab_widget.addTab(self.data_table, "数据表")
        
        # 添加选项卡变化处理
        self.tab_widget.currentChanged.connect(self.on_tab_changed)
        
        main_layout.addWidget(self.tab_widget, 1)  # 1是伸展因子，使其占据更多空间
        
        # 连接信号和槽
        self.connect_button.clicked.connect(self.connect_to_esp32)
        self.disconnect_button.clicked.connect(self.disconnect_from_esp32)
        self.send_button.clicked.connect(self.send_message)
        
        self.mode_default_btn.clicked.connect(lambda: self.send_mode_command("MODE_DEFAULT"))
        self.mode_realtime_btn.clicked.connect(lambda: self.send_mode_command("MODE_REALTIME"))
        self.mode_history_btn.clicked.connect(lambda: self.send_mode_command("MODE_HISTORY"))
        
        self.flash_info_btn.clicked.connect(lambda: self.send_command("FLASH_INFO"))
        self.flash_read_btn.clicked.connect(self.send_flash_read_command)
        self.flash_erase_btn.clicked.connect(lambda: self.send_command("FLASH_ERASE"))
        self.clear_data_btn.clicked.connect(self.clear_all_data)
        self.export_data_btn.clicked.connect(self.export_data)
        
        # 初始化Worker和WebSocket客户端
        self.worker = Worker()
        self.worker.message_received.connect(self.append_log)
        self.worker.data_received.connect(self.process_data)
        self.worker.connection_status.connect(self.update_status)
        self.client = None
        
        # 设置按钮状态
        self.update_button_states(False)

    # 用户手动切换选项卡时锁定自动切换功能
    def on_tab_changed(self, _index):
        self.tab_lock = True
        self.append_log("已锁定选项卡，数据将在后台更新")

    def disconnect_from_esp32(self):
        """断开与ESP32的连接"""
        if self.client:
            try:
                self.client.close()
                self.append_log("🔌 主动断开连接")
            except Exception as e:
                self.append_log(f"❌ 断开连接时出错: {e}")

        self.client = None
        self.update_button_states(False)
        self.status_label.setText("已断开连接")

        # 重置帧率显示
        self.frame_count = 0
        self.frame_rate_label.setText("帧率: 0.0 fps")

    def update_frame_rate(self):
        """更新帧率显示"""
        current_time = time.time()
        time_diff = current_time - self.last_frame_time

        if time_diff >= 1.0:  # 每秒更新一次
            fps = self.frame_count / time_diff
            self.frame_rate_label.setText(f"帧率: {fps:.1f} fps")
            self.frame_count = 0
            self.last_frame_time = current_time

    def clear_all_data(self):
        """清空所有显示数据"""
        self.data_graphs.clear_data()
        self.data_table.clear_data()
        self.log_output.clear()

        # 重置帧率计数
        self.frame_count = 0
        self.frame_rate_label.setText("帧率: 0.0 fps")

        self.append_log("📝 已清空所有显示数据")

    def export_data(self):
        """导出数据功能"""
        # 获取当前选中的选项卡
        current_tab = self.tab_widget.currentWidget()

        # 根据当前选项卡确定默认文件名和导出类型
        if current_tab == self.data_graphs:
            default_filename = f"图表数据_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
            dialog_title = "导出图表数据"
        elif current_tab == self.data_table:
            default_filename = f"表格数据_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
            dialog_title = "导出表格数据"
        else:
            # 如果在日志选项卡，默认导出图表数据
            default_filename = f"图表数据_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
            dialog_title = "导出图表数据"
            current_tab = self.data_graphs

        # 打开文件保存对话框
        filename, _ = QFileDialog.getSaveFileName(
            self,
            dialog_title,
            default_filename,
            "CSV文件 (*.csv);;所有文件 (*.*)"
        )

        if filename:
            # 确保文件扩展名为.csv
            if not filename.lower().endswith('.csv'):
                filename += '.csv'

            # 根据选择的选项卡导出相应数据
            if current_tab == self.data_graphs:
                success, message = self.data_graphs.export_to_csv(filename)
            else:  # current_tab == self.data_table
                success, message = self.data_table.export_to_csv(filename)

            # 显示结果
            if success:
                self.append_log(f"✅ {message}")
                QMessageBox.information(self, "导出成功", message)
            else:
                self.append_log(f"❌ {message}")
                QMessageBox.warning(self, "导出失败", message)

    def connect_to_esp32(self):
        # 如果已经有客户端但已断开连接，先清理
        if self.client is not None:
            self.client = None
            self.append_log("🔄 重置连接状态")

        try:
            ip = socket.gethostbyname("esp32.local")
            uri = f"ws://{ip}:8080/ws"
            self.append_log(f"🌐 正在连接: {uri}")
            self.client = WebSocketClient(uri, self.worker)
            self.update_button_states(True)
        except Exception as e:
            self.append_log(f"❌ mDNS 解析失败: {e}")
            self.append_log("尝试使用默认IP连接...")
            try:
                uri = "ws://192.168.4.1:8080/ws"  # 使用ESP32的默认AP模式IP
                self.append_log(f"🌐 正在连接: {uri}")
                self.client = WebSocketClient(uri, self.worker)
                self.update_button_states(True)
            except Exception as e2:
                self.append_log(f"❌ 连接失败: {e2}")
                # 连接失败时确保按钮状态正确
                self.update_button_states(False)

    def send_message(self):
        msg = self.send_input.text()
        if self.client:
            self.client.send(msg)
            self.send_input.clear()
        else:
            self.append_log("❌ 未连接")

    def send_command(self, command):
        if self.client:
            self.client.send(command)
        else:
            self.append_log("❌ 未连接")

    def send_mode_command(self, mode):
        if self.client:
            self.client.send(mode)
            self.append_log(f"🔄 切换到{mode}模式")
            # 切换模式时重置选项卡锁定
            self.tab_lock = False
        else:
            self.append_log("❌ 未连接")

    def send_flash_read_command(self):
        if not self.client:
            self.append_log("❌ 未连接")
            return
            
        try:
            index = int(self.read_index_input.text() or "0")
            count = int(self.read_count_input.text() or "1")
            command = f"FLASH_READ:{index},{count}"
            self.client.send(command)
            # 读取Flash数据时重置选项卡锁定
            self.tab_lock = False
        except ValueError:
            self.append_log("❌ 索引和数量必须为整数")

    def append_log(self, text):
        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        formatted_text = f"[{timestamp}] {text}"
        self.log_output.append(formatted_text)

        # 限制日志行数以避免内存问题
        if self.log_output.document().blockCount() > 1000:
            cursor = self.log_output.textCursor()
            cursor.movePosition(cursor.Start)
            cursor.movePosition(cursor.Down, cursor.KeepAnchor, 100)
            cursor.removeSelectedText()

    def update_status(self, text):
        self.status_label.setText(text)
        if "连接成功" in text:
            self.update_button_states(True)
        elif "连接关闭" in text or "错误" in text:
            # 连接关闭或出错时，禁用操作按钮但保持连接按钮可用
            self.update_button_states(False)
            self.append_log("连接已断开，可以点击\"连接 ESP32\"按钮重新连接")

            # 重置帧率显示
            self.frame_count = 0
            self.frame_rate_label.setText("帧率: 0.0 fps")

    def update_button_states(self, connected):
        # 启用/禁用按钮
        self.mode_default_btn.setEnabled(connected)
        self.mode_realtime_btn.setEnabled(connected)
        self.mode_history_btn.setEnabled(connected)
        self.flash_info_btn.setEnabled(connected)
        self.flash_read_btn.setEnabled(connected)
        self.flash_erase_btn.setEnabled(connected)
        self.send_button.setEnabled(connected)

        # 连接和断开按钮的状态相反
        self.connect_button.setEnabled(not connected)
        self.disconnect_button.setEnabled(connected)

        # 清空数据按钮始终可用
        self.clear_data_btn.setEnabled(True)

    def process_data(self, data):
        # 增加帧计数用于帧率计算
        self.frame_count += 1

        # 修复中文前缀显示问题
        if data["prefix"] and not data["prefix"].isascii():
            try:
                # 尝试检测前缀中的"实时"或"历史"中文字符
                if "\xe5\xae\x9e\xe6\x97\xb6" in data["prefix"] or "实时" in data["prefix"]:
                    data["prefix"] = "实时"
                elif "\xe5\x8e\x86\xe5\x8f\xb2" in data["prefix"] or "历史" in data["prefix"]:
                    data["prefix"] = "历史"
            except:
                pass  # 忽略任何处理错误
        
        # 更新图表
        self.data_graphs.update_data(data)
        
        # 更新表格
        self.data_table.add_data(data)
        
        # 仅当选项卡未锁定时才自动切换
        if not self.tab_lock:
            if "实时" in data["prefix"]:
                self.tab_widget.setCurrentWidget(self.data_graphs)
            elif "历史" in data["prefix"]:
                self.tab_widget.setCurrentWidget(self.data_table)


if __name__ == "__main__":
    app = QApplication(sys.argv)
    
    # 设置全局样式
    app.setStyle("Fusion")
    
    win = MainWindow()
    win.show()
    sys.exit(app.exec_())