#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <opencv2/opencv.hpp>
using namespace cv;
using namespace std;

// 配置参数结构体，集中管理常量
struct Config {
    static constexpr const char* DEVICE = "127.0.0.1:16384";
    static constexpr float FIXED_THRESHOLD = 0.25f;
    static constexpr const char* SCREENSHOT_PATH = "./screenshot.png";
    static constexpr const char* UI_TEMPLATE_DIR = "./ui/";
    static constexpr int RETRY_ATTEMPTS = 1;  // 重试次数
    static constexpr int CLICK_DELAY_MS = 500000;  // 点击间隔微秒
    static constexpr int PROCESS_DELAY_SEC = 5;    // 流程间隔秒数
};

// 全局坐标变量（仅在匹配成功后有效）
int GLOBAL_X = -1;
int GLOBAL_Y = -1;

/**
 * @param cmd 要执行的命令
 * @return 0表示成功，-1表示失败
 */
int execute_command(const char* cmd) {
    if (!cmd || strlen(cmd) == 0) return -1;
    FILE* pipe = _popen(cmd, "r");  // Windows 下使用 _popen
    if (!pipe) return -1;
    
    char buffer[128];
    while (!feof(pipe)) {
        fgets(buffer, 128, pipe);  // 读取输出（可根据需要处理）
    }
    
    int ret = _pclose(pipe);  // Windows 下使用 _pclose
    return ret == 0 ? 0 : -1;
}

/**
 * 通过ADB执行点击操作
 * @param x 点击x坐标
 * @param y 点击y坐标
 */
void adb_click(int x, int y) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "adb -s %s shell input tap %d %d", 
             Config::DEVICE, x, y);
    
    if (execute_command(cmd) == 0) {
        printf("ADB点击成功：(%d, %d)\n", x, y);
    } else {
        printf("ADB点击失败：(%d, %d)\n", x, y);
    }
}

/**
 * 通过ADB执行滑动操作
 * @param x1 起始x坐标
 * @param y1 起始y坐标
 * @param x2 目标x坐标
 * @param y2 目标y坐标
 * @param duration 滑动持续时间(秒)
 */
void adb_swipe(int x1, int y1, int x2, int y2, float duration) {
    int duration_ms = static_cast<int>(duration * 1000);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "adb -s %s shell input swipe %d %d %d %d %d",
             Config::DEVICE, x1, y1, x2, y2, duration_ms);
    
    if (execute_command(cmd) == 0) {
        printf("ADB滑动成功：(%d,%d) -> (%d,%d) 耗时%.1f秒\n",
               x1, y1, x2, y2, duration);
    } else {
        printf("ADB滑动失败：(%d,%d) -> (%d,%d)\n",
               x1, y1, x2, y2);
    }
}

/**
 * 检查文件是否存在
 * @param path 文件路径
 * @return 1表示存在，0表示不存在
 */
int file_exists(const char* path) {
    if (!path) return 0;
    struct stat buffer;
    return (stat(path, &buffer) == 0) ? 1 : 0;
}

/**
 * 检查截图文件是否存在
 * @return 1表示存在，0表示不存在
 */
int check_screenshot() {
    if (file_exists(Config::SCREENSHOT_PATH)) {
        printf("截图文件存在：%s\n", Config::SCREENSHOT_PATH);
        return 1;
    } else {
        printf("警告：截图文件不存在 %s\n", Config::SCREENSHOT_PATH);
        return 0;
    }
}

/**
 * 执行ADB连接设备并获取截图（单次调用）
 * @return 0表示成功，1表示失败
 */
int take_screenshot_once() {
    char adb_screenshot_cmd[200];
    const char* screenshot_name = Config::SCREENSHOT_PATH;
    int ret;
    
    snprintf(adb_screenshot_cmd, sizeof(adb_screenshot_cmd), 
             "adb -s %s shell screencap -p /sdcard/%s && adb -s %s pull /sdcard/%s . && adb -s %s shell rm /sdcard/%s",
             Config::DEVICE, screenshot_name, Config::DEVICE, screenshot_name, Config::DEVICE, screenshot_name);
    
    printf("正在截取最新屏幕...\n");
    ret = system(adb_screenshot_cmd);
    if (ret != 0) {
        printf("截图失败！\n");
        return 1;
    }
    
    printf("最新截图已保存为：%s\n", screenshot_name);
    return 0;
}

/**
 * 模板匹配（支持重试机制，每次匹配前刷新截图）
 * @param img_model_path 模板图片文件名
 * @return 1表示匹配成功，0表示失败
 */
int match_template(const char* img_model_path) {
    if (!img_model_path) return 0;
    char full_model_path[256];
    snprintf(full_model_path, sizeof(full_model_path), 
             "%s%s", Config::UI_TEMPLATE_DIR, img_model_path);
    
    // 检查模板文件是否存在
    if (!file_exists(full_model_path)) {
        printf("模板文件不存在：%s\n", full_model_path);
        GLOBAL_X = -1;
        GLOBAL_Y = -1;
        return 0;
    }
    
    // 读取模板
    Mat img_model = imread(full_model_path);
    if (img_model.empty()) {
        printf("无法读取模板：%s\n", full_model_path);
        GLOBAL_X = -1;
        GLOBAL_Y = -1;
        return 0;
    }
    
    int model_h = img_model.rows;
    int model_w = img_model.cols;
    
    for (int attempt = 0; attempt <= Config::RETRY_ATTEMPTS; attempt++) {
        // 每次匹配前先刷新截图
        if (take_screenshot_once() != 0) {
            printf("截图刷新失败（尝试 %d/%d）\n", attempt + 1, Config::RETRY_ATTEMPTS + 1);
            usleep(Config::CLICK_DELAY_MS);
            continue;
        }
        
        if (!check_screenshot()) {
            usleep(Config::CLICK_DELAY_MS);
            continue;
        }
        
        // 读取最新截图
        Mat img = imread(Config::SCREENSHOT_PATH);
        if (img.empty()) {
            printf("无法读取最新截图 %s（尝试 %d/%d）\n", 
                   Config::SCREENSHOT_PATH, attempt + 1, Config::RETRY_ATTEMPTS + 1);
            usleep(Config::CLICK_DELAY_MS);
            continue;
        }
        
        Mat result;
        matchTemplate(img, img_model, result, TM_SQDIFF_NORMED);
        double min_val;
        Point min_loc;
        minMaxLoc(result, &min_val, nullptr, &min_loc, nullptr);
        
        if (min_val <= Config::FIXED_THRESHOLD) {
            GLOBAL_X = min_loc.x + model_w / 2;  // 计算中心坐标
            GLOBAL_Y = min_loc.y + model_h / 2;
            printf("%s 匹配成功（尝试 %d）：坐标 (%d, %d)，匹配值 %.4f\n",
                   img_model_path, attempt + 1, GLOBAL_X, GLOBAL_Y, min_val);
            return 1;
        } else {
            printf("%s 匹配失败（尝试 %d）：匹配值 %.4f > 阈值 %.2f\n",
                   img_model_path, attempt + 1, min_val, Config::FIXED_THRESHOLD);
            if (attempt < Config::RETRY_ATTEMPTS) {
                usleep(Config::CLICK_DELAY_MS);
            }
        }
    }
    
    printf("%s 所有尝试均失败\n", img_model_path);
    GLOBAL_X = -1;
    GLOBAL_Y = -1;
    return 0;
}

/**
 * 处理模板列表
 * @param templates 模板文件名数组
 * @param count 模板数量
 * @param click_after_match 匹配成功后是否点击
 */
void process_templates(const char* templates[], int count, int click_after_match) {
    printf("使用ADB连接设备：%s，匹配阈值：%.2f\n", 
           Config::DEVICE, Config::FIXED_THRESHOLD);
    
    for (int i = 0; i < count; i++) {
        const char* template_name = templates[i];
        printf("\n===== 处理模板：%s =====\n", template_name);
        
        int found = match_template(template_name);
        
        if (click_after_match && found && GLOBAL_X != -1 && GLOBAL_Y != -1) {
            printf("准备点击坐标：(%d, %d)\n", GLOBAL_X, GLOBAL_Y);
            adb_click(GLOBAL_X, GLOBAL_Y);
            sleep(1);
        } else if (!found) {
            printf("跳过 %s 点击（无有效坐标）\n", template_name);
        }
    }
}

void process_grassman() {
    const char* templates[] = {"caoman.png"};
    process_templates(templates, 1, 1);
}

void process_matching() {
    const char* templates[] = {"jingong.png", "sousuo.png"};
    process_templates(templates, 2, 1);
}

void process_gohome() {
    const char* templates[] = {"jieshu.png", "queding.png", "huiying.png"};
    process_templates(templates, 3, 1);
}

void process_queen() {
    const char* templates[] = {"nvhuang.png"};
    process_templates(templates, 1, 1);
}

void process_fullking() {
    const char* templates[] = {"manwang.png"};
    process_templates(templates, 1, 1);
}

void process_braveking() {
    const char* templates[] = {"yongwang.png"};
    process_templates(templates, 1, 1);
}

void process_soiltu() {
    const char* templates[] = {"runtu.png"};
    process_templates(templates, 1, 1);
}

void process_eagle() {
    const char* templates[] = {"cangying.png"};
    process_templates(templates, 1, 1);
}

void process_dragon() {
    const char* templates[] = {"feilong.png"};
    process_templates(templates, 1, 1);
}

void process_thunder() {
    const char* templates[] = {"leidian.png"};
    process_templates(templates, 1, 1);
}

int process_bird() {
    const char* templates[] = {"tianniao.png"};
    process_templates(templates, 1, 0);
    return (GLOBAL_X != -1 && GLOBAL_Y != -1) ? 1 : 0;
}

/**
 * 执行内部点击序列
 * @param sequence 点击坐标序列
 * @param count 序列长度
 */
void execute_click_sequence(const int sequence[][2], int count) {
    for (int i = 0; i < count; i++) {
        adb_click(sequence[i][0], sequence[i][1]);
        usleep(Config::CLICK_DELAY_MS / 2);  // 缩短序列内点击间隔
    }
}

int init_device_connection() {
    char adb_connect_cmd[100];
    int ret;
    
    snprintf(adb_connect_cmd, sizeof(adb_connect_cmd), 
             "adb connect %s", Config::DEVICE);
    
    printf("正在连接设备：%s...\n", Config::DEVICE);
    ret = system(adb_connect_cmd);
    if (ret != 0) {
        printf("设备连接失败！\n");
        return 1;
    }
    
    printf("设备连接成功\n");
    return 0;
}

void main_loop() {
    const int inner_clicks[7][2] = {
        {670, 345}, {978, 170}, {412, 584}, {1519, 112},
        {1773, 304}, {1833, 1091}, {737, 1085}
    };
    const int click_count = sizeof(inner_clicks) / sizeof(inner_clicks[0]);
    
    for (int i = 0; i < 999; i++) {
        printf("\n===== 主循环第 %d 轮 =====\n", i + 1);
        
        process_matching();
        sleep(Config::PROCESS_DELAY_SEC);
        
        process_thunder();
        int bird_found = process_bird();
        if (bird_found) {
            for (int j = 0; j < 11; j++) {
                printf("第 %d/11 次点击天鸟\n", j + 1);
                adb_click(GLOBAL_X, GLOBAL_Y);
                usleep(Config::CLICK_DELAY_MS);
            }
        } else {
            printf("未找到天鸟，跳过点击\n");
        }
        
        process_queen();
        adb_click(670, 345);
        sleep(1);
        process_fullking();
        adb_click(670, 345);
        sleep(1);
        process_braveking();
        adb_click(670, 345);
        sleep(1);
        process_soiltu();
        adb_click(670, 345);
        sleep(1);
        process_eagle();
        adb_click(670, 345);
        sleep(1);
        
        for (int j = 0; j < 8; j++) {
            process_grassman();
            process_dragon();
            
            execute_click_sequence(inner_clicks, click_count);
            printf("第 %d/8 次点击序列完成\n", j + 1);
            usleep(Config::CLICK_DELAY_MS);
        }
        
        sleep(30);
        process_gohome();
        sleep(Config::PROCESS_DELAY_SEC);
    }
}

int main() {
    if (init_device_connection() != 0) {
        printf("错误：设备连接失败，程序将退出\n");
        return 1;
    }
    
    main_loop();
    
    printf("\n所有操作执行完毕\n");
    return 0;
}