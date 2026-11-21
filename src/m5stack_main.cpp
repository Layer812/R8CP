#include <Arduino.h>
/*
 * Created by Layer8
 * M5Stack Cardputer entry point and hardware abstraction layer for RONTO8.
 */
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <Preferences.h>

extern "C" {
#include "p8_emu.h"
#include "p8_parser.h"

// Expose internal functions
extern uint16_t m_colors[32];
extern void render_sounds(int16_t *buffer, int total_samples);
}

extern unsigned m_actual_fps;

static constexpr int MAX_BROWSER_ENTRIES = 256;
static constexpr int MAX_BROWSER_BUF_SIZE = 4096;

static char* g_browser_name_buf = nullptr;
static uint16_t* g_browser_name_offsets = nullptr;
static bool* g_browser_is_dir = nullptr;
static int g_browser_buf_used = 0;

static String g_browser_path = "/";
static volatile bool g_scan_complete = false;
static int g_browser_count = 0;
static int g_browser_cursor = 0;
static int g_cursor_history[10] = {0};
static int g_depth = 0;
static volatile bool g_emulator_ready = false;
static volatile bool g_emulator_init_failed = false;
static volatile bool g_audio_task_started = false;

int g_volume = 255;

extern "C" {
    char g_last_error_message[256] = {0};
    bool g_emulator_crashed = false;
    char g_cart_title[32] = {0};
    char g_cart_author[32] = {0};
}

static bool is_rom_file(const String& name)
{
    String lower = name;
    lower.toLowerCase();
    return lower.endsWith(".p8") || lower.endsWith(".png");
}

static void scan_browser_directory(const String& path)
{
    g_browser_count = 0;
    g_browser_cursor = 0;
    g_browser_buf_used = 0;

    File dir = SD.open(path.c_str());
    if (!dir || !dir.isDirectory())
        return;

    if (path != "/" && path != "/sd" && path != "/sd/" && g_browser_count < MAX_BROWSER_ENTRIES) {
        strcpy(g_browser_name_buf + g_browser_buf_used, "..");
        g_browser_name_offsets[g_browser_count] = g_browser_buf_used;
        g_browser_is_dir[g_browser_count] = true;
        g_browser_buf_used += 3;
        g_browser_count++;
    }

    while (true) {
        File entry = dir.openNextFile();
        if (!entry)
            break;

        String name = entry.name();
        bool is_dir = entry.isDirectory();
        entry.close();

        if (name == "." || name == "..") {
            continue;
        }

        if (is_dir || is_rom_file(name)) {
            if (g_browser_count < MAX_BROWSER_ENTRIES) {
                int len = name.length();
                if (g_browser_buf_used + len + 1 < MAX_BROWSER_BUF_SIZE) {
                    strcpy(g_browser_name_buf + g_browser_buf_used, name.c_str());
                    g_browser_name_offsets[g_browser_count] = g_browser_buf_used;
                    g_browser_is_dir[g_browser_count] = is_dir;
                    g_browser_buf_used += len + 1;
                    g_browser_count++;
                }
            }
        }
    }

    dir.close();
}

static void draw_browser()
{
    uint16_t r4_yellow = M5Cardputer.Display.color565(253, 192, 0);
    M5Cardputer.Display.fillScreen(r4_yellow);
    M5Cardputer.Display.setTextColor(TFT_BLACK, r4_yellow);

    M5Cardputer.Display.setCursor(5, 5);
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.println("R8 ROM BROWSER");
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(5, 25);
    M5Cardputer.Display.println(g_browser_path);

    M5Cardputer.Display.drawRect(2, 34, 236, 83, TFT_BLACK);
    
    M5Cardputer.Display.setTextSize(2);
    int visible = 5;
    int start = 0;
    if (g_browser_count > visible) {
        if (g_browser_cursor >= visible)
            start = g_browser_cursor - visible + 1;
        if (start + visible > g_browser_count)
            start = g_browser_count - visible;
    }

    int y_pos = 36;
    for (int i = start; i < start + visible && i < g_browser_count; i++) {
        const bool selected = (i == g_browser_cursor);
        if (selected)
            M5Cardputer.Display.fillRect(4, y_pos, 232, 16, TFT_BLACK);
        M5Cardputer.Display.setTextColor(selected ? r4_yellow : TFT_BLACK,
                                         selected ? TFT_BLACK : r4_yellow);
        M5Cardputer.Display.setCursor(6, y_pos);
        const char* entry_name = g_browser_name_buf + g_browser_name_offsets[i];
        if (g_browser_is_dir[i]) {
            M5Cardputer.Display.print("[");
            M5Cardputer.Display.print(entry_name);
            M5Cardputer.Display.println("]");
        } else {
            M5Cardputer.Display.println(entry_name);
        }
        y_pos += 16;
    }

    M5Cardputer.Display.setTextColor(TFT_BLACK, r4_yellow);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(5, 118);
    M5Cardputer.Display.println("UP: W/; DOWN: S/. ENTER: Open");
    M5Cardputer.Display.setCursor(5, 126);
    M5Cardputer.Display.println("BS: Back");
}

static String browse_for_rom()
{
    g_browser_name_buf = (char*)malloc(MAX_BROWSER_BUF_SIZE);
    g_browser_name_offsets = (uint16_t*)malloc(MAX_BROWSER_ENTRIES * sizeof(uint16_t));
    g_browser_is_dir = (bool*)malloc(MAX_BROWSER_ENTRIES * sizeof(bool));

    g_browser_path = "/";
    scan_browser_directory(g_browser_path);
    draw_browser();

    String selected_file = "";

    bool browser_key_armed = false;
    uint32_t browser_released_at_ms = 0;

    while (true) {
        M5Cardputer.update();
        M5Cardputer.Keyboard.updateKeyList();
        M5Cardputer.Keyboard.updateKeysState();

        auto& ks = M5Cardputer.Keyboard.keysState();

        bool up = false, down = false, left = false, right = false;
        bool enter = ks.enter || ks.space;
        bool bs = ks.del;

        for (char c : ks.word) {
            if (c == 'w' || c == 'W' || c == 'u' || c == 'U' || c == ';') up = true;
            if (c == 's' || c == 'S' || c == '.') down = true;
            if (c == 'a' || c == 'A' || c == ',') left = true;
            if (c == 'd' || c == 'D' || c == '/') right = true;
            if (c == '\n' || c == '\r') enter = true;
        }

        const uint32_t now_ms = millis();
        const bool key_down = M5Cardputer.Keyboard.isPressed() != 0;
        if (!key_down) {
            if (browser_released_at_ms == 0) browser_released_at_ms = now_ms;
            if ((uint32_t)(now_ms - browser_released_at_ms) >= 40)
                browser_key_armed = true;
        } else {
            browser_released_at_ms = 0;
        }
        const bool pressed_once = key_down && browser_key_armed;
        if (pressed_once) browser_key_armed = false;
        if (pressed_once) {
        if (up && g_browser_cursor > 0) {
            g_browser_cursor--;
            draw_browser();
        } else if (down && g_browser_cursor + 1 < g_browser_count) {
            g_browser_cursor++;
            draw_browser();
        } else if (left) {
            g_browser_cursor -= 5;
            if (g_browser_cursor < 0) g_browser_cursor = 0;
            draw_browser();
        } else if (right) {
            g_browser_cursor += 5;
            if (g_browser_cursor >= g_browser_count) g_browser_cursor = g_browser_count - 1;
            if (g_browser_cursor < 0) g_browser_cursor = 0;
            draw_browser();
        } else if (bs) {
            if (g_browser_path != "/" && g_browser_path != "/sd" && g_browser_path != "/sd/") {
                int last_slash = g_browser_path.lastIndexOf('/', g_browser_path.length() - 2);
                if (last_slash >= 0) {
                    g_browser_path = g_browser_path.substring(0, last_slash + 1);
                } else {
                    g_browser_path = "/";
                }
                if (g_depth > 0) {
                    g_depth--;
                    g_browser_cursor = g_cursor_history[g_depth];
                } else {
                    g_browser_cursor = 0;
                }
                scan_browser_directory(g_browser_path);
                draw_browser();
            }
        } else if (enter && g_browser_count > 0) {
            if (g_browser_is_dir[g_browser_cursor]) {
                const char* entry_name = g_browser_name_buf + g_browser_name_offsets[g_browser_cursor];
                if (strcmp(entry_name, "..") == 0) {
                    int last_slash = g_browser_path.lastIndexOf('/', g_browser_path.length() - 2);
                    if (last_slash >= 0) {
                        g_browser_path = g_browser_path.substring(0, last_slash + 1);
                    } else {
                        g_browser_path = "/";
                    }
                    if (g_depth > 0) {
                        g_depth--;
                        g_browser_cursor = g_cursor_history[g_depth];
                    }
                } else {
                    if (g_depth < 10) {
                        g_cursor_history[g_depth] = g_browser_cursor;
                        g_depth++;
                    }
                    String next = g_browser_path;
                    if (!next.endsWith("/")) next += "/";
                    next += entry_name;
                    g_browser_path = next;
                    g_browser_cursor = 0;
                }
                scan_browser_directory(g_browser_path);
                draw_browser();
            } else {
                const char* entry_name = g_browser_name_buf + g_browser_name_offsets[g_browser_cursor];
                selected_file = g_browser_path + (g_browser_path.endsWith("/") ? "" : "/") + entry_name;
                break;
            }
        }
        }
        delay(12);
    }

    free(g_browser_name_buf);
    free(g_browser_name_offsets);
    free(g_browser_is_dir);
    g_browser_name_buf = nullptr;
    g_browser_name_offsets = nullptr;
    g_browser_is_dir = nullptr;

    return selected_file;
}

static void audio_task(void *pvParameters)
{
    const int sample_count = 256;
    int16_t *audio_buf[3] = {nullptr, nullptr, nullptr};
    for (int i = 0; i < 3; ++i)
        audio_buf[i] = (int16_t *)malloc(sample_count * sizeof(int16_t));

    if (!audio_buf[0] || !audio_buf[1] || !audio_buf[2]) {
        for (int i = 0; i < 3; ++i)
            if (audio_buf[i]) free(audio_buf[i]);
        vTaskDelete(NULL);
        return;
    }

    int current_buf = 0;
    while (1) {
        render_sounds(audio_buf[current_buf], sample_count);
        while (!M5Cardputer.Speaker.playRaw(audio_buf[current_buf], sample_count, 22050, false, 1, 0)) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        current_buf = (current_buf + 1) % 3;
    }
}

static void emulator_init_task(void *pvParameters)
{
    const char *path = (const char *)pvParameters;
    if (!path) {
        g_emulator_init_failed = true;
        vTaskDelete(NULL);
        return;
    }

    Serial.printf("[boot] starting init: %s\n", path);
    int ret = p8_init_file_with_param(path, NULL);
    Serial.printf("[boot] init result=%d\n", ret);

    if (ret == 0) {
        g_emulator_ready = true;
        g_emulator_init_failed = false;
    } else {
        g_emulator_ready = false;
        g_emulator_init_failed = true;
    }

    free((void *)path);
    vTaskDelete(NULL);
}

extern "C" void m5stack_update_input()
{
    uint8_t mask = 0;
    auto& ks = M5Cardputer.Keyboard.keysState();

    if (ks.enter) mask |= 32; // X
    if (ks.space) mask |= 16; // O

    for (char c : ks.word) {
        if (c == 'a' || c == 'A' || c == 0x03 || c == ',') mask |= 1; // LEFT
        if (c == 'd' || c == 'D' || c == 0x04 || c == '/') mask |= 2; // RIGHT
        if (c == 'w' || c == 'W' || c == 0x01 || c == ';') mask |= 4; // UP
        if (c == 's' || c == 'S' || c == 0x02 || c == '.') mask |= 8; // DOWN
        if (c == '\n' || c == '\r' || c == 'x' || c == 'X' || c == 'k' || c == 'K') mask |= 32; // X
        if (c == ' ' || c == 'z' || c == 'Z' || c == 'v' || c == 'V' || c == 'n' || c == 'N') mask |= 16; // O
        if (c == 0x1b || c == '\t' || c == 'm' || c == 'M' || c == 'p' || c == 'P') mask |= 64; // START
    }

    m_buttons[0] = mask;
}

extern "C" void m5stack_suspend_frame_buf(void) { }
extern "C" void m5stack_resume_frame_buf(void) { }

extern "C" void p8_render()
{
    if (!m_memory) return;
    uint8_t* vram = &m_memory[0x6000]; // VRAM starts at 0x6000
    
    const int LINE_BUFFER_HEIGHT = 8;
    uint16_t line_buffer[128 * LINE_BUFFER_HEIGHT];
    
    int start_x = (M5Cardputer.Display.width() - 128) / 2;
    int start_y = (M5Cardputer.Display.height() - 128) / 2;

    M5Cardputer.Display.startWrite();
    for (int block_y = 0; block_y < 128; block_y += LINE_BUFFER_HEIGHT) {
        for (int y = 0; y < LINE_BUFFER_HEIGHT; y++) {
            int absolute_y = block_y + y;
            for (int x = 0; x < 128; x++) {
                uint8_t c;
                if (x % 2 == 0) {
                    c = vram[(absolute_y * 64) + (x / 2)] & 0x0F;
                } else {
                    c = vram[(absolute_y * 64) + (x / 2)] >> 4;
                }
                line_buffer[y * 128 + x] = m_colors[c];
            }
        }
        M5Cardputer.Display.pushImage(start_x, start_y + block_y, 128, LINE_BUFFER_HEIGHT, line_buffer);
    }
    M5Cardputer.Display.endWrite();
}

void show_ronto8_splash() {
    uint16_t r4_yellow = M5Cardputer.Display.color565(253, 192, 0);
    
    M5Cardputer.Display.fillScreen(r4_yellow);
    M5Cardputer.Display.setTextColor(TFT_BLACK, r4_yellow);
    
    M5Cardputer.Display.setTextSize(4);
    M5Cardputer.Display.setCursor(10, 10);
    M5Cardputer.Display.print("R8");

    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setCursor(10, 45);
    M5Cardputer.Display.print("RONTO8 ");
    M5Cardputer.Display.print("CARDPUTER");

    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(10, 65);
    M5Cardputer.Display.println("RONTO8,a nu wave in the pico8 console.");
    M5Cardputer.Display.setCursor(10, 75);
    M5Cardputer.Display.println("Powering beyond hardware limitations");
    M5Cardputer.Display.setCursor(10, 85);
    M5Cardputer.Display.println("and adv acceler8 the portable coding.");
    M5Cardputer.Display.setCursor(10, 120);
    M5Cardputer.Display.print("(C)RONTO8. BASED ON FEMTO8 & ZEPTO8.");

    while (true) {
        M5Cardputer.update();
        M5Cardputer.Keyboard.updateKeyList();
        M5Cardputer.Keyboard.updateKeysState();
        
        if (M5Cardputer.Keyboard.isPressed()) {
            break;
        }

        if (millis() % 1000 < 500) {
            M5Cardputer.Display.setTextColor(TFT_BLACK, r4_yellow);
        } else {
            M5Cardputer.Display.setTextColor(r4_yellow, r4_yellow);
        }
        M5Cardputer.Display.setTextSize(1);
        M5Cardputer.Display.setCursor(60, 105);
        M5Cardputer.Display.print("  PRESS ANY KEY");

        delay(50);
    }
    M5Cardputer.Display.setCursor(60, 105);
    M5Cardputer.Display.print("  loading...     ");
//    M5Cardputer.Display.fillScreen(r4_yellow);
}

void setup()
{
    Serial.begin(115200);
    delay(3000); // Wait for serial monitor to connect
    Serial.println("--- M5CARDPUTER BOOT ---");
    
    extern unsigned char *m_memory;
    if (!m_memory) {
        m_memory = (uint8_t *)malloc(MEMORY_SIZE); // 64KB
        if (m_memory) {
            memset(m_memory, 0, MEMORY_SIZE);
            Serial.printf("[Memory] m_memory (64KB) allocated as early singleton.\n");
        } else {
            Serial.printf("[Memory] FATAL: Failed to allocate m_memory at boot!\n");
        }
    }

    auto cfg = M5.config();
    cfg.internal_mic = false;
    cfg.internal_spk = true;
    cfg.internal_imu = false;
    cfg.internal_rtc = false;
    M5Cardputer.begin(cfg, true);

    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(255);
    
    M5Cardputer.Display.setRotation(0);
    M5Cardputer.Display.setColorDepth(16);
    M5Cardputer.Display.setSwapBytes(true);
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5Cardputer.Display.setTextSize(1);

    SPI.begin(40, 39, 14, 12);
    int retries = 0;
    while (!SD.begin(12, SPI, 15000000) && retries < 5) {
        M5Cardputer.Display.println("SD mount failed, retrying...");
        delay(1000);
        retries++;
    }

    if (retries >= 5) {
        M5Cardputer.Display.println("FATAL: SD completely failed.");
        while(1) { delay(100); }
    }

    show_ronto8_splash();

    String found_file = browse_for_rom();
    if (found_file != "") {
        if (!found_file.startsWith("/")) found_file = "/" + found_file;
        
        uint16_t r4_yellow = M5Cardputer.Display.color565(253, 192, 0);
        M5Cardputer.Display.fillScreen(r4_yellow);
        M5Cardputer.Display.setTextColor(TFT_BLACK, r4_yellow);
        M5Cardputer.Display.setTextSize(2);
        M5Cardputer.Display.setCursor(5, 5);
        M5Cardputer.Display.print("R8");
        M5Cardputer.Display.setTextSize(1);
        M5Cardputer.Display.setCursor(5, 30);
        M5Cardputer.Display.print("DIR:");
        M5Cardputer.Display.setCursor(5, 40);
        M5Cardputer.Display.print(" ;.,/");
        M5Cardputer.Display.setCursor(5, 50);
        M5Cardputer.Display.print(" WASD");
        M5Cardputer.Display.setCursor(5, 65);
        M5Cardputer.Display.print("O: Z/N");
        M5Cardputer.Display.setCursor(5, 75);
        M5Cardputer.Display.print("X: X/M");
        M5Cardputer.Display.setCursor(5, 90);
        M5Cardputer.Display.print("START:");
        M5Cardputer.Display.setCursor(5, 100);
        M5Cardputer.Display.print(" P/ENT");
        M5Cardputer.Display.setCursor(5, 110);
        M5Cardputer.Display.print("BACK:BS");
        M5Cardputer.Display.setCursor(5, 120);
        M5Cardputer.Display.print("VOL: +/-");

        M5Cardputer.Display.fillRect(56, 3, 128, 128, TFT_BLACK);
        M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5Cardputer.Display.setCursor(60, 60);
        M5Cardputer.Display.print("Loading...");

        String vfs_path = "/sd" + found_file;
        char* param_path = strdup(vfs_path.c_str());
        g_emulator_ready = false;
        g_emulator_init_failed = false;

        xTaskCreatePinnedToCore(emulator_init_task, "emulator_init_task", 16384, (void *)param_path, 2, NULL, 1);

        return;
    } else {
        M5Cardputer.Display.println("No ROM selected.");
    }

    while(1) { delay(100); }
}

void loop() {
    M5Cardputer.update();
    M5Cardputer.Keyboard.updateKeyList();
    M5Cardputer.Keyboard.updateKeysState();

    auto& ks = M5Cardputer.Keyboard.keysState();

    if (g_emulator_crashed || g_emulator_init_failed) {
        static bool shown = false;
        if (!shown) {
            shown = true;
            uint16_t box_color = M5Cardputer.Display.color565(200, 0, 0);
            M5Cardputer.Display.fillRect(20, 20, 200, 95, TFT_BLACK);
            M5Cardputer.Display.drawRect(20, 20, 200, 95, box_color);
            M5Cardputer.Display.drawRect(21, 21, 198, 93, box_color);
            M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
            M5Cardputer.Display.setTextSize(1);
            M5Cardputer.Display.setCursor(25, 25);
            M5Cardputer.Display.println("EMULATOR ERROR:");
            M5Cardputer.Display.setCursor(25, 40);
            if (strlen(g_last_error_message) > 0) {
                M5Cardputer.Display.println(g_last_error_message);
            } else {
                M5Cardputer.Display.println("ROM init failed or crashed.");
            }
            M5Cardputer.Display.setCursor(25, 90);
            M5Cardputer.Display.println("Press BS or ENTER to reboot");
        }
        if (ks.del || ks.enter || ks.space) {
            ESP.restart();
        }
    } else if (g_emulator_ready && !g_audio_task_started) {
        uint16_t r4_yellow = M5Cardputer.Display.color565(253, 192, 0);
        M5Cardputer.Display.fillRect(185, 0, 55, 135, r4_yellow);
        M5Cardputer.Display.setTextColor(TFT_BLACK, r4_yellow);
        M5Cardputer.Display.setTextSize(1);
        String t = g_cart_title;
        String a = g_cart_author;
        if (t.length() > 9) t = t.substring(0, 9);
        if (a.length() > 9) a = a.substring(0, 9);
        M5Cardputer.Display.setCursor(186, 5);
        M5Cardputer.Display.println(t);
        if (a.length() > 0) {
            M5Cardputer.Display.setCursor(186, 15);
            M5Cardputer.Display.println(a);
        }

        xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, NULL, configMAX_PRIORITIES - 1, NULL, 0);
        g_audio_task_started = true;
    } else if (g_emulator_ready) {
        if (ks.del) {
            ESP.restart();
        }
        
        static int s_volume = 255;
        for (char c : ks.word) {
            if (c == '+' || c == '=') {
                s_volume += 15;
                if (s_volume > 255) s_volume = 255;
                M5Cardputer.Speaker.setVolume(s_volume);
            } else if (c == '-' || c == '_') {
                s_volume -= 15;
                if (s_volume < 0) s_volume = 0;
                M5Cardputer.Speaker.setVolume(s_volume);
            }
        }

        p8_step();
    }

    delay(1);
}