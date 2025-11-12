#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QThread>
#include <QMutex>
#include <QMessageBox>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <opencv2/opencv.hpp>

using namespace cv;
using namespace std;

// 全局日志输出锁
QMutex g_log_mutex;
// 任务控制标志
bool g_task_running = false;

// 配置参数结构体（支持GUI修改）
struct Config {
    static QString DEVICE;
    static float FIXED_THRESHOLD;
    static const char* SCREENSHOT_PATH;
    static const char* UI_TEMPLATE_DIR;
    static int RETRY_ATTEMPTS;
    static int CLICK_DELAY_MS;
    static int PROCESS_DELAY_SEC;
};

// 初始化静态成员
QString Config::DEVICE = "127.0.0.1:16384";
float Config::FIXED_THRESHOLD = 0.25f;
const char* Config::SCREENSHOT_PATH = "./screenshot.png";
const char* Config::UI_TEMPLATE_DIR = "./ui/";
int Config::RETRY_ATTEMPTS = 1;
int Config::CLICK_DELAY_MS = 500000;
int Config::PROCESS_DELAY_SEC = 5;

// 全局坐标变量
int GLOBAL_X = -1;
int GLOBAL_Y = -1;

// 日志输出函数（支持GUI显示）
void log_output(const QString& msg) {
    QMutexLocker locker(&g_log_mutex);
    printf("%s\n", msg.toUtf8().constData());
    // 发送日志信号给GUI（后续通过信号槽实现）
}

// 原有核心功能函数（适配Config结构体修改）
int execute_command(const char* cmd) {
    if (!cmd || strlen(cmd) == 0) return -1;
#ifdef _WIN32
    FILE* pipe = _popen(cmd, "r");
#else
    FILE* pipe = popen(cmd, "r");
#endif
    if (!pipe) return -1;
    
    char buffer[128];
    while (!feof(pipe)) {
        fgets(buffer, 128, pipe);
    }
    
#ifdef _WIN32
    int ret = _pclose(pipe);
#else
    int ret = pclose(pipe);
#endif
    return ret == 0 ? 0 : -1;
}

void adb_click(int x, int y) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "adb -s %s shell input tap %d %d", 
             Config::DEVICE.toUtf8().constData(), x, y);
    
    if (execute_command(cmd) == 0) {
        log_output(QString("ADB点击成功：(%1, %2)").arg(x).arg(y));
    } else {
        log_output(QString("ADB点击失败：(%1, %2)").arg(x).arg(y));
    }
}

void adb_swipe(int x1, int y1, int x2, int y2, float duration) {
    int duration_ms = static_cast<int>(duration * 1000);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "adb -s %s shell input swipe %d %d %d %d %d",
             Config::DEVICE.toUtf8().constData(), x1, y1, x2, y2, duration_ms);
    
    if (execute_command(cmd) == 0) {
        log_output(QString("ADB滑动成功：(%1,%2) -> (%3,%4) 耗时%.1f秒")
                  .arg(x1).arg(y1).arg(x2).arg(y2).arg(duration));
    } else {
        log_output(QString("ADB滑动失败：(%1,%2) -> (%3,%4)")
                  .arg(x1).arg(y1).arg(x2).arg(y2));
    }
}

int file_exists(const char* path) {
    if (!path) return 0;
    struct stat buffer;
    return (stat(path, &buffer) == 0) ? 1 : 0;
}

int check_screenshot() {
    if (file_exists(Config::SCREENSHOT_PATH)) {
        log_output(QString("截图文件存在：%1").arg(Config::SCREENSHOT_PATH));
        return 1;
    } else {
        log_output(QString("警告：截图文件不存在 %1").arg(Config::SCREENSHOT_PATH));
        return 0;
    }
}

int take_screenshot_once() {
    char adb_screenshot_cmd[200];
    const char* screenshot_name = Config::SCREENSHOT_PATH;
    int ret;
    
    snprintf(adb_screenshot_cmd, sizeof(adb_screenshot_cmd), 
             "adb -s %s shell screencap -p /sdcard/%s && adb -s %s pull /sdcard/%s . && adb -s %s shell rm /sdcard/%s",
             Config::DEVICE.toUtf8().constData(), screenshot_name,
             Config::DEVICE.toUtf8().constData(), screenshot_name,
             Config::DEVICE.toUtf8().constData(), screenshot_name);
    
    log_output("正在截取最新屏幕...");
    ret = system(adb_screenshot_cmd);
    if (ret != 0) {
        log_output("截图失败！");
        return 1;
    }
    
    log_output(QString("最新截图已保存为：%1").arg(screenshot_name));
    return 0;
}

int match_template(const char* img_model_path) {
    if (!img_model_path) return 0;
    char full_model_path[256];
    snprintf(full_model_path, sizeof(full_model_path), 
             "%s%s", Config::UI_TEMPLATE_DIR, img_model_path);
    
    if (!file_exists(full_model_path)) {
        log_output(QString("模板文件不存在：%1").arg(full_model_path));
        GLOBAL_X = -1;
        GLOBAL_Y = -1;
        return 0;
    }
    
    Mat img_model = imread(full_model_path);
    if (img_model.empty()) {
        log_output(QString("无法读取模板：%1").arg(full_model_path));
        GLOBAL_X = -1;
        GLOBAL_Y = -1;
        return 0;
    }
    
    int model_h = img_model.rows;
    int model_w = img_model.cols;
    
    for (int attempt = 0; attempt <= Config::RETRY_ATTEMPTS; attempt++) {
        if (take_screenshot_once() != 0) {
            log_output(QString("截图刷新失败（尝试 %1/%2）").arg(attempt+1).arg(Config::RETRY_ATTEMPTS+1));
            usleep(Config::CLICK_DELAY_MS);
            continue;
        }
        
        if (!check_screenshot()) {
            usleep(Config::CLICK_DELAY_MS);
            continue;
        }
        
        Mat img = imread(Config::SCREENSHOT_PATH);
        if (img.empty()) {
            log_output(QString("无法读取最新截图 %1（尝试 %2/%3）")
                      .arg(Config::SCREENSHOT_PATH).arg(attempt+1).arg(Config::RETRY_ATTEMPTS+1));
            usleep(Config::CLICK_DELAY_MS);
            continue;
        }
        
        Mat result;
        matchTemplate(img, img_model, result, TM_SQDIFF_NORMED);
        double min_val;
        Point min_loc;
        minMaxLoc(result, &min_val, nullptr, &min_loc, nullptr);
        
        if (min_val <= Config::FIXED_THRESHOLD) {
            GLOBAL_X = min_loc.x + model_w / 2;
            GLOBAL_Y = min_loc.y + model_h / 2;
            log_output(QString("%1 匹配成功（尝试 %2）：坐标 (%3, %4)，匹配值 %.4f")
                      .arg(img_model_path).arg(attempt+1).arg(GLOBAL_X).arg(GLOBAL_Y).arg(min_val));
            return 1;
        } else {
            log_output(QString("%1 匹配失败（尝试 %2）：匹配值 %.4f > 阈值 %.2f")
                      .arg(img_model_path).arg(attempt+1).arg(min_val).arg(Config::FIXED_THRESHOLD));
            if (attempt < Config::RETRY_ATTEMPTS) {
                usleep(Config::CLICK_DELAY_MS);
            }
        }
    }
    
    log_output(QString("%1 所有尝试均失败").arg(img_model_path));
    GLOBAL_X = -1;
    GLOBAL_Y = -1;
    return 0;
}

void process_templates(const char* templates[], int count, int click_after_match) {
    log_output(QString("使用ADB连接设备：%1，匹配阈值：%.2f")
              .arg(Config::DEVICE).arg(Config::FIXED_THRESHOLD));
    
    for (int i = 0; i < count; i++) {
        const char* template_name = templates[i];
        log_output(QString("\n===== 处理模板：%1 =====\n").arg(template_name));
        
        int found = match_template(template_name);
        
        if (click_after_match && found && GLOBAL_X != -1 && GLOBAL_Y != -1) {
            log_output(QString("准备点击坐标：(%1, %2)").arg(GLOBAL_X).arg(GLOBAL_Y));
            adb_click(GLOBAL_X, GLOBAL_Y);
            sleep(1);
        } else if (!found) {
            log_output(QString("跳过 %1 点击（无有效坐标）").arg(template_name));
        }
    }
}

// 所有process_xxx函数保持不变（略，与原代码一致）
void process_grassman() { const char* t[] = {"caoman.png"}; process_templates(t, 1, 1); }
void process_matching() { const char* t[] = {"jingong.png", "sousuo.png"}; process_templates(t, 2, 1); }
void process_gohome() { const char* t[] = {"jieshu.png", "queding.png", "huiying.png"}; process_templates(t, 3, 1); }
void process_queen() { const char* t[] = {"nvhuang.png"}; process_templates(t, 1, 1); }
void process_fullking() { const char* t[] = {"manwang.png"}; process_templates(t, 1, 1); }
void process_braveking() { const char* t[] = {"yongwang.png"}; process_templates(t, 1, 1); }
void process_soiltu() { const char* t[] = {"runtu.png"}; process_templates(t, 1, 1); }
void process_eagle() { const char* t[] = {"cangying.png"}; process_templates(t, 1, 1); }
void process_dragon() { const char* t[] = {"feilong.png"}; process_templates(t, 1, 1); }
void process_thunder() { const char* t[] = {"leidian.png"}; process_templates(t, 1, 1); }

int process_bird() {
    const char* templates[] = {"tianniao.png"};
    process_templates(templates, 1, 0);
    return (GLOBAL_X != -1 && GLOBAL_Y != -1) ? 1 : 0;
}

void execute_click_sequence(const int sequence[][2], int count) {
    for (int i = 0; i < count; i++) {
        adb_click(sequence[i][0], sequence[i][1]);
        usleep(Config::CLICK_DELAY_MS / 2);
    }
}

int init_device_connection() {
    char adb_connect_cmd[100];
    int ret;
    
    snprintf(adb_connect_cmd, sizeof(adb_connect_cmd), 
             "adb connect %s", Config::DEVICE.toUtf8().constData());
    
    log_output(QString("正在连接设备：%1...").arg(Config::DEVICE));
    ret = system(adb_connect_cmd);
    if (ret != 0) {
        log_output("设备连接失败！");
        return 1;
    }
    
    log_output("设备连接成功");
    return 0;
}

// 任务线程类（避免GUI阻塞）
class TaskThread : public QThread {
    Q_OBJECT
public:
    void run() override {
        const int inner_clicks[7][2] = {
            {670, 345}, {978, 170}, {412, 584}, {1519, 112},
            {1773, 304}, {1833, 1091}, {737, 1085}
        };
        const int click_count = sizeof(inner_clicks) / sizeof(inner_clicks[0]);
        
        for (int i = 0; i < 999 && g_task_running; i++) {
            log_output(QString("\n===== 主循环第 %1 轮 =====\n").arg(i+1));
            
            process_matching();
            sleep(Config::PROCESS_DELAY_SEC);
            
            process_thunder();
            int bird_found = process_bird();
            if (bird_found) {
                for (int j = 0; j < 11 && g_task_running; j++) {
                    log_output(QString("第 %1/11 次点击天鸟").arg(j+1));
                    adb_click(GLOBAL_X, GLOBAL_Y);
                    usleep(Config::CLICK_DELAY_MS);
                }
            } else {
                log_output("未找到天鸟，跳过点击");
            }
            
            process_queen(); adb_click(670, 345); sleep(1);
            process_fullking(); adb_click(670, 345); sleep(1);
            process_braveking(); adb_click(670, 345); sleep(1);
            process_soiltu(); adb_click(670, 345); sleep(1);
            process_eagle(); adb_click(670, 345); sleep(1);
            
            for (int j = 0; j < 8 && g_task_running; j++) {
                process_grassman();
                process_dragon();
                execute_click_sequence(inner_clicks, click_count);
                log_output(QString("第 %1/8 次点击序列完成").arg(j+1));
                usleep(Config::CLICK_DELAY_MS);
            }
            
            if (g_task_running) {
                sleep(30);
                process_gohome();
                sleep(Config::PROCESS_DELAY_SEC);
            }
        }
        log_output("任务已停止");
    }
};

// GUI主窗口类
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(QWidget* parent = nullptr) : QMainWindow(parent) {
        init_ui();
        init_signals();
        thread = new TaskThread();
    }
    
    ~MainWindow() {
        g_task_running = false;
        thread->wait();
        delete thread;
    }

private slots:
    void on_connect_clicked() {
        // 更新配置参数
        Config::DEVICE = edt_device->text();
        Config::FIXED_THRESHOLD = spin_threshold->value();
        Config::RETRY_ATTEMPTS = spin_retry->value();
        Config::CLICK_DELAY_MS = spin_click_delay->value() * 1000;  // 转微秒
        Config::PROCESS_DELAY_SEC = spin_process_delay->value();
        
        // 连接设备
        int ret = init_device_connection();
        if (ret == 0) {
            QMessageBox::information(this, "成功", "设备连接成功！");
        } else {
            QMessageBox::critical(this, "失败", "设备连接失败，请检查ADB和设备地址！");
        }
    }
    
    void on_start_clicked() {
        if (g_task_running) return;
        
        g_task_running = true;
        thread->start();
        btn_start->setEnabled(false);
        btn_stop->setEnabled(true);
        log_output("任务已启动");
    }
    
    void on_stop_clicked() {
        g_task_running = false;
        btn_start->setEnabled(true);
        btn_stop->setEnabled(false);
        log_output("正在停止任务...");
    }
    
    void append_log(const QString& msg) {
        QMutexLocker locker(&g_log_mutex);
        txt_log->append(msg);
        // 自动滚动到底部
        txt_log->moveCursor(QTextCursor::End);
    }

private:
    void init_ui() {
        set