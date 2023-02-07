#include "battery.h"
#include <WiFi.h>
#include "esp_task_wdt.h"
#include "esp_camera.h"
#include "PoE_CAM.h"
#include <esp_heap_caps.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_log.h"
#include "ext_info.h"
#include <Wire.h>
#include "bmm8563.h"


char wifi_ssid[36];
char wifi_pwd[36];

char* TIMERCAM_CONFIG_WIFI_SSID = " ";
char* TIMERCAM_CONFIG_WIFI_PASSWORD = " ";
int TIMERCAM_CONFIG_WAKETIME = -1;
int TIMERCAM_CONFIG_PICSIZE = -1;

const char CAM_LOGO[] =
" _____ _                      ____                \n"
"|_   _(_)_ __ ___   ___ _ __ / ___|__ _ _ __ ___  \n"
"  | | | | '_ ` _ \\ / _ \\ '__| |   / _` | '_ ` _ \\ \n"
"  | | | | | | | | |  __/ |  | |__| (_| | | | | | |\n"
"  |_| |_|_| |_| |_|\\___|_|   \\____\\__,_|_| |_| |_|\n";

bool restart              = false;
volatile bool init_finish = false;

void frame_post_callback(uint8_t cmd) {
    if (restart && (cmd == (kSetDeviceMode | 0x80))) {
        esp_restart();
    } else if (cmd == (kRestart | 0x80)) {
        esp_restart();
    } else if (cmd == (kSetWiFi | 0x80)) {
    }
}

void frame_recv_callback(int cmd_in, const uint8_t* data, int len) {
    if (init_finish == false) {
        return;
    }

    Serial.printf("Recv cmd %d\r\n", cmd_in);

    if (cmd_in == kRestart) {
        uint8_t respond_data = 0;
        uart_frame_send(cmd_in | 0x80, &respond_data, 1, false);
        return;
    }

    if (cmd_in == kSetDeviceMode || GetDeviceMode() != data[0]) {
        restart = true;
    }

    uint8_t* respond_buff;
    int respond_len = 0;
    respond_buff    = DealConfigMsg(cmd_in, data, len, &respond_len);
    uart_frame_send(cmd_in | 0x80, respond_buff, respond_len, false);
}

void start_uart_server(void) {
    camera_fb_t* fb = NULL;
    size_t _jpg_buf_len;
    uint8_t* _jpg_buf;
    static int64_t last_frame = 0;
    if (!last_frame) {
        last_frame = esp_timer_get_time();
    }

    esp_task_wdt_init(1, true);
    esp_task_wdt_add(NULL);

    while (true) {
        fb = esp_camera_fb_get();
        esp_task_wdt_reset();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            continue;
        }

        _jpg_buf_len = fb->len;
        _jpg_buf     = fb->buf;

        if (!(_jpg_buf[_jpg_buf_len - 1] != 0xd9 ||
              _jpg_buf[_jpg_buf_len - 2] != 0xd9)) {
            esp_camera_fb_return(fb);
            continue;
        }

        uart_frame_send(kImage, _jpg_buf, _jpg_buf_len, true);
        esp_camera_fb_return(fb);

        int64_t fr_end     = esp_timer_get_time();
        int64_t frame_time = fr_end - last_frame;
        last_frame         = fr_end;
        frame_time /= 1000;
    }
    last_frame = 0;
}


#define PIN_POWER_HOLD  33
#define PIN_LED         2

void start_timing_server() {
    // TimerCamConfig_t* config = GetTimerCamCfg();
    // if (config->wifi_ssid[0] == '\0') {
    //     Serial.printf("empty wifi config\n");
    //     return;
    // }
    // wifi_init_sta(config->wifi_ssid, config->wifi_pwd);
    // printf("%s, %s", config->wifi_ssid, config->wifi_pwd);




    if (TIMERCAM_CONFIG_WIFI_SSID[0] == '\0')
    {
        Serial.printf("Empty wifi ssid, please check config\n");
        while (1);
    }

    Serial.printf("Wifi init...\n");
    wifi_init_sta(TIMERCAM_CONFIG_WIFI_SSID, TIMERCAM_CONFIG_WIFI_PASSWORD);

    while (wifi_wait_connect(100) == false) {
        Serial.printf("Connecting...\n");
        delay(100);
    }
    Serial.println("Success\n");

    bool result = false;
    uint8_t mac[6];
    String a;
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    esp_base_mac_addr_set(mac);

    // c4dd57b9a9408c4862544d3b0d2631da
    char* token  = NULL;
    uint32_t len = 0;
    char* data   = NULL;
    asprintf(&token, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    for (;;) {
        result = get_token("http://camera.m5stack.com/token_get", &data, &len);

        if (result == false) {
            Serial.println("Got Token Error");
            vTaskDelay(1000);
            continue;
        }

        if (data[1] == 'T') {
            free(data);
            result = get_token("http://camera.m5stack.com/token", &data, &len);
        }

        if (result == true) {
            SetToken(data + 1);
            // Serial.printf("Got Token: %s\n", data + 1);
            break;
        }
    }

    for (;;) {
        // config = GetTimerCamCfg();
        // vTaskDelay(config->TimingTime * 1000);

        Serial.printf("Capturing image...\n");
        camera_fb_t* fb = esp_camera_fb_get();

        Serial.printf("Posting image to server...");
        const char* url = "http://camera.m5stack.com/timer-cam/image";
        result = http_post_image(url, token, fb->buf, fb->len, 3000);
        Serial.printf("%s\n", result ? "success!" : "failed!");
        if (result)
        {
            Serial.printf("-- Image posted to url: http://api.m5stack.com:5003/timer-cam/image?tok=\n");
            Serial.printf("-- Please add your token at the end of the url\n");
            Serial.printf("-- You can get your token in M5Burner\n\n");
        }
        else {
            Serial.printf("Rebooting\n");
            esp_restart();
        }

        esp_camera_fb_return(fb);

        /* Go to sleep */
        Serial.printf("TimerCam will be wake up after %d seconds\n", TIMERCAM_CONFIG_WAKETIME);
        Serial.printf("Power off :)\n\n");
        // delay(TIMERCAM_CONFIG_WAKETIME * 1000);



        /* Setup RTC wake up */
        bmm8563_setTimerIRQ(TIMERCAM_CONFIG_WAKETIME);
        delay(500);

        /* Power off */
        bat_disable_output();

        /* If usb is connected */
        esp_deep_sleep(TIMERCAM_CONFIG_WAKETIME * 1000000);
        esp_deep_sleep_start();
    }
}



void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // disable   detector



    /* Hold power */
    bat_init();
    bmm8563_init();
    
    Serial.begin(115200);




    /* Light up led */
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, 1);



    Serial.printf("\n%s\n", CAM_LOGO);
    
    // uart_init(Ext_PIN_1, Ext_PIN_2);
    // uart_init(1, 3);
    // uart_init(1, 3);



    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGI(TAG, "Erase nvs flash");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    /* Get config from NVS */
	// ESP_LOGW(TAG, "LOADING_CONFIG_INFO...");
    Serial.printf("Reading config info...\n");
	ExtInfoInitAddr(0x3ff000);
    ExtInfoGetString("ssid", &TIMERCAM_CONFIG_WIFI_SSID);
	ExtInfoGetString("pwd", &TIMERCAM_CONFIG_WIFI_PASSWORD);
    ExtInfoGetInt("wake_time", &TIMERCAM_CONFIG_WAKETIME);
    ExtInfoGetInt("image_size", &TIMERCAM_CONFIG_PICSIZE);
    // ESP_LOGW(TAG, "-- TIMERCAM_CONFIG_WIFI_SSID: %s", TIMERCAM_CONFIG_WIFI_SSID);
    // ESP_LOGW(TAG, "-- TIMERCAM_CONFIG_WIFI_PASSWORD: %s", TIMERCAM_CONFIG_WIFI_PASSWORD);
    // ESP_LOGW(TAG, "-- TIMERCAM_CONFIG_WAKETIME: %d", TIMERCAM_CONFIG_WAKETIME);
    // ESP_LOGW(TAG, "-- TIMERCAM_CONFIG_PICSIZE: %d", TIMERCAM_CONFIG_PICSIZE);
    Serial.printf("-- Wifi SSID: %s\n", TIMERCAM_CONFIG_WIFI_SSID);
    Serial.printf("-- Wake time: %d\n", TIMERCAM_CONFIG_WAKETIME);
    Serial.printf("-- Image size: %d\n", TIMERCAM_CONFIG_PICSIZE);




    Serial.printf("Camera init...\n");
    camera_config_t config;
    config.pin_pwdn = CAM_PIN_PWDN, config.pin_reset = CAM_PIN_RESET;
    config.pin_xclk     = CAM_PIN_XCLK;
    config.pin_sscb_sda = CAM_PIN_SIOD;
    config.pin_sscb_scl = CAM_PIN_SIOC;
    config.pin_d7       = CAM_PIN_D7;
    config.pin_d6       = CAM_PIN_D6;
    config.pin_d5       = CAM_PIN_D5;
    config.pin_d4       = CAM_PIN_D4;
    config.pin_d3       = CAM_PIN_D3;
    config.pin_d2       = CAM_PIN_D2;
    config.pin_d1       = CAM_PIN_D1;
    config.pin_d0       = CAM_PIN_D0;
    config.pin_vsync    = CAM_PIN_VSYNC;
    config.pin_href     = CAM_PIN_HREF;
    config.pin_pclk     = CAM_PIN_PCLK;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    // config.frame_size   = FRAMESIZE_UXGA;
    config.frame_size   = (framesize_t)TIMERCAM_CONFIG_PICSIZE;
    config.jpeg_quality = 16;
    config.fb_count     = 2;
    // config.fb_count     = 1;
    esp_err_t err       = esp_camera_init(&config);

    if (err) {
        Serial.printf("Camera init failed\n");
        delay(1000);
        esp_restart();
    }

    Serial.printf("Camera Init Success\n");
    delay(1000);

    // while (1)
    //     delay(1000);

    // InitTimerCamConfig();
    InitCamFun();

    sensor_t* s = esp_camera_sensor_get();
    s->set_vflip(s, 1);
    s->set_hmirror(s, 0);


    



    init_finish = true;




    Serial.printf("Starting post server\n");
    start_timing_server();
    return;



    
    if (GetDeviceMode() == kUart) {
        Serial.println("UART MODE");
        start_uart_server();
    } else if (GetDeviceMode() == kTiming) {
        Serial.println("TIMER MODE");
        start_timing_server();
    } else if (GetDeviceMode() == kWifiSta) {
        while (GetWifiConfig(wifi_ssid, wifi_pwd) == false) {
            // uint8_t error_code = kWifiMsgError;
            // uart_frame_send(kErrorOccur, &error_code, 1, false);
            Serial.printf("get wifi config error\n");
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        Serial.printf("ssid: %s, pwd: %s\r\n", wifi_ssid, wifi_pwd);
        Serial.println("STA MODE");
        start_webserver(wifi_ssid, wifi_pwd);
    }
}

void loop() {
    delay(100);
    digitalWrite(2, HIGH);
    delay(100);
    digitalWrite(2, LOW);
}








