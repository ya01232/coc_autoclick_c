#include <stdio.h>
#include <stdlib.h>

int main() {
    char adb_connect_cmd[100];
    char adb_screenshot_cmd[200];
    const char* screenshot_name = "screenshot.png";
    int ret;

    snprintf(adb_connect_cmd, sizeof(adb_connect_cmd), 
             "adb connect 127.0.0.1:16384");
    
    snprintf(adb_screenshot_cmd, sizeof(adb_screenshot_cmd), 
             "adb -s 127.0.0.1:16384 shell screencap -p /sdcard/%s && adb -s 127.0.0.1:16384 pull /sdcard/%s . && adb -s 127.0.0.1:16384 shell rm /sdcard/%s",
             screenshot_name, screenshot_name, screenshot_name);

    printf("Connecting to 127.0.0.1:16384...\n");
    ret = system(adb_connect_cmd);
    if (ret != 0) {
        printf("Failed to connect device!\n");
        return 1;
    }

    printf("Taking screenshot and saving...\n");
    ret = system(adb_screenshot_cmd);
    if (ret != 0) {
        printf("Failed to take screenshot!\n");
        return 1;
    }

    printf("Screenshot saved as: %s\n", screenshot_name);
    return 0;
}