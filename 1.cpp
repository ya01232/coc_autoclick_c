#include <windows.h>
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
    static constexpr int RETRY_ATTEMPTS = 1;
    static constexpr int CLICK_DELAY_MS = 500000;
    static constexpr int PROCESS_DELAY_SEC = 5;
};

// 全局坐标变量（仅在匹配成功后有效）
int GLOBAL_X = -1;
int GLOBAL_Y = -1;

/**
 * 执行系统命令并返回结果（Windows 兼容版）
 */
int execute_command(const char* cmd) {
    if (!cmd || strlen(cmd) == 0) return -1;
    FILE* pipe = _popen(cmd, "r");
    if (!pipe) return -1;
    
    char buffer[128];
    while (!feof(pipe)) {
        fgets(buffer, 128, pipe);
    }
    
    int ret = _pclose(pipe);
    return ret == 0 ? 0 : -1;
}

/**
 * 通过ADB执行点击操作
 */
void adb_click(int x, int y) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "adb -s %s shell input tap %d %d", 
             Config::DEVICE, x, y);
    
    if (execute_command(cmd) == 0) {
        printf("ADB click success: (%d, %d)\n", x, y);
    } else {
        printf("ADB click failed: (%d, %d)\n", x, y);
    }
}

/**
 * 通过ADB执行滑动操作
 */
void adb_swipe(int x1, int y1, int x2, int y2, float duration) {
    int duration_ms = static_cast<int>(duration * 1000);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "adb -s %s shell input swipe %d %d %d %d %d",
             Config::DEVICE, x1, y1, x2, y2, duration_ms);
    
    if (execute_command(cmd) == 0) {
        printf("ADB swipe success: (%d,%d) -> (%d,%d) duration:%.1fs\n",
               x1, y1, x2, y2, duration);
    } else {
        printf("ADB swipe failed: (%d,%d) -> (%d,%d)\n",
               x1, y1, x2, y2);
    }
}

/**
 * 检查文件是否存在
 */
int file_exists(const char* path) {
    if (!path) return 0;
    struct stat buffer;
    return (stat(path, &buffer) == 0) ? 1 : 0;
}

/**
 * 检查截图文件是否存在
 */
int check_screenshot() {
    if (file_exists(Config::SCREENSHOT_PATH)) {
        printf("Screenshot exists: %s\n", Config::SCREENSHOT_PATH);
        return 1;
    } else {
        printf("Warning: Screenshot not found %s\n", Config::SCREENSHOT_PATH);
        return 0;
    }
}

/**
 * 执行ADB连接设备并获取截图（单次调用）
 */
int take_screenshot_once() {
    char adb_screenshot_cmd[200];
    const char* screenshot_name = Config::SCREENSHOT_PATH;
    int ret;
    
    snprintf(adb_screenshot_cmd, sizeof(adb_screenshot_cmd), 
             "adb -s %s shell screencap -p /sdcard/%s && adb -s %s pull /sdcard/%s . && adb -s %s shell rm /sdcard/%s",
             Config::DEVICE, screenshot_name, Config::DEVICE, screenshot_name, Config::DEVICE, screenshot_name);
    
    printf("Capturing latest screen...\n");
    ret = system(adb_screenshot_cmd);
    if (ret != 0) {
        printf("Screenshot failed!\n");
        return 1;
    }
    
    printf("Latest screenshot saved as: %s\n", screenshot_name);
    return 0;
}

/**
 * 模板匹配（支持重试机制，每次匹配前刷新截图）
 */
int match_template(const char* img_model_path) {
    if (!img_model_path) return 0;
    char full_model_path[256];
    snprintf(full_model_path, sizeof(full_model_path), 
             "%s%s", Config::UI_TEMPLATE_DIR, img_model_path);
    
    if (!file_exists(full_model_path)) {
        printf("Template file not found: %s\n", full_model_path);
        GLOBAL_X = -1;
        GLOBAL_Y = -1;
        return 0;
    }
    
    Mat img_model = imread(full_model_path);
    if (img_model.empty()) {
        printf("Failed to read template: %s\n", full_model_path);
        GLOBAL_X = -1;
        GLOBAL_Y = -1;
        return 0;
    }
    
    int model_h = img_model.rows;
    int model_w = img_model.cols;
    
    for (int attempt = 0; attempt <= Config::RETRY_ATTEMPTS; attempt++) {
        if (take_screenshot_once() != 0) {
            printf("Screenshot refresh failed (attempt %d/%d)\n", attempt + 1, Config::RETRY_ATTEMPTS + 1);
            usleep(Config::CLICK_DELAY_MS);
            continue;
        }
        
        if (!check_screenshot()) {
            usleep(Config::CLICK_DELAY_MS);
            continue;
        }
        
        Mat img = imread(Config::SCREENSHOT_PATH);
        if (img.empty()) {
            printf("Failed to read latest screenshot %s (attempt %d/%d)\n", 
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
            GLOBAL_X = min_loc.x + model_w / 2;
            GLOBAL_Y = min_loc.y + model_h / 2;
            printf("%s match success (attempt %d): position (%d, %d), match value %.4f\n",
                   img_model_path, attempt + 1, GLOBAL_X, GLOBAL_Y, min_val);
            return 1;
        } else {
            printf("%s match failed (attempt %d): match value %.4f > threshold %.2f\n",
                   img_model_path, attempt + 1, min_val, Config::FIXED_THRESHOLD);
            if (attempt < Config::RETRY_ATTEMPTS) {
                usleep(Config::CLICK_DELAY_MS);
            }
        }
    }
    
    printf("%s all attempts failed\n", img_model_path);
    GLOBAL_X = -1;
    GLOBAL_Y = -1;
    return 0;
}

/**
 * 处理模板列表
 */
void process_templates(const char* templates[], int count, int click_after_match) {
    printf("Using ADB device: %s, match threshold: %.2f\n", 
           Config::DEVICE, Config::FIXED_THRESHOLD);
    
    for (int i = 0; i < count; i++) {
        const char* template_name = templates[i];
        printf("\n===== Processing template: %s =====\n", template_name);
        
        int found = match_template(template_name);
        
        if (click_after_match && found && GLOBAL_X != -1 && GLOBAL_Y != -1) {
            printf("Preparing to click position: (%d, %d)\n", GLOBAL_X, GLOBAL_Y);
            adb_click(GLOBAL_X, GLOBAL_Y);
            sleep(1);
        } else if (!found) {
            printf("Skip %s click (no valid position)\n", template_name);
        }
    }
}

// 各个处理函数（保持功能不变，日志改为英文避免编码问题）
void process_grassman() {  // 草莽
    const char* templates[] = {"caoman.png"};
    process_templates(templates, 1, 1);
}

void process_matching() {  // 匹配
    const char* templates[] = {"jingong.png", "sousuo.png"};
    process_templates(templates, 2, 1);
}

void process_gohome() {  // 回家
    const char* templates[] = {"jieshu.png", "queding.png", "huiying.png"};
    process_templates(templates, 3, 1);
}

void process_queen() {  // 女皇
    const char* templates[] = {"nvhuang.png"};
    process_templates(templates, 1, 1);
}

void process_fullking() {  // 满王
    const char* templates[] = {"manwang.png"};
    process_templates(templates, 1, 1);
}

void process_braveking() {  // 勇王
    const char* templates[] = {"yongwang.png"};
    process_templates(templates, 1, 1);
}

void process_soiltu() {  // 闰土
    const char* templates[] = {"runtu.png"};
    process_templates(templates, 1, 1);
}

void process_eagle() {  // 苍鹰
    const char* templates[] = {"cangying.png"};
    process_templates(templates, 1, 1);
}

void process_dragon() {  // 飞龙
    const char* templates[] = {"feilong.png"};
    process_templates(templates, 1, 1);
}

void process_thunder() {  // 雷电
    const char* templates[] = {"leidian.png"};
    process_templates(templates, 1, 1);
}

int process_bird() {  // 天鸟
    const char* templates[] = {"tianniao.png"};
    process_templates(templates, 1, 0);
    return (GLOBAL_X != -1 && GLOBAL_Y != -1) ? 1 : 0;
}

/**
 * 执行内部点击序列
 */
void execute_click_sequence(const int sequence[][2], int count) {
    for (int i = 0; i < count; i++) {
        adb_click(sequence[i][0], sequence[i][1]);
        usleep(Config::CLICK_DELAY_MS / 2);
    }
}

/**
 * 初始化设备连接（仅执行一次）
 */
int init_device_connection() {
    char adb_connect_cmd[100];
    int ret;
    
    snprintf(adb_connect_cmd, sizeof(adb_connect_cmd), 
             "adb connect %s", Config::DEVICE);
    
    printf("Connecting to device: %s...\n", Config::DEVICE);
    ret = system(adb_connect_cmd);
    if (ret != 0) {
        printf("Device connection failed!\n");
        return 1;
    }
    
    printf("Device connected successfully\n");
    return 0;
}

/**
 * 主循环逻辑
 */
void main_loop() {
    const int inner_clicks[7][2] = {
        {670, 345}, {978, 170}, {412, 584}, {1519, 112},
        {1773, 304}, {1833, 1091}, {737, 1085}
    };
    const int click_count = sizeof(inner_clicks) / sizeof(inner_clicks[0]);
    
    for (int i = 0; i < 999; i++) {
        printf("\n===== Main loop round %d =====\n", i + 1);
        
        process_matching();
        sleep(Config::PROCESS_DELAY_SEC);
        
        // Thunder bird cannon process
        process_thunder();
        int bird_found = process_bird();
        if (bird_found) {
            for (int j = 0; j < 11; j++) {
                printf("Clicking bird %d/11\n", j + 1);
                adb_click(GLOBAL_X, GLOBAL_Y);
                usleep(Config::CLICK_DELAY_MS);
            }
        } else {
            printf("Bird not found, skip clicking\n");
        }
        
        // Deploy king process
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
        
        // Loop click operation
        for (int j = 0; j < 8; j++) {
            process_grassman();
            process_dragon();
            
            execute_click_sequence(inner_clicks, click_count);
            printf("Click sequence completed %d/8\n", j + 1);
            usleep(Config::CLICK_DELAY_MS);
        }
        
        // Return home after delay
        sleep(30);
        process_gohome();
        sleep(Config::PROCESS_DELAY_SEC);
    }
}

int main() {
    // Windows console UTF-8 encoding fix
    #ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    #endif

    // Initialize device connection
    if (init_device_connection() != 0) {
        printf("Error: Device connection failed, program will exit\n");
        return 1;
    }
    
    // Execute main loop
    main_loop();
    
    printf("\nAll operations completed successfully\n");
    return 0;
}