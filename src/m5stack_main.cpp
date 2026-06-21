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

extern "C" {
#include "p8_emu.h"
#include "p8_parser.h"

// Expose internal functions
extern uint16_t m_colors[32];
extern void render_sounds(int16_t *buffer, int total_samples);
}

extern unsigned m_actual_fps;

static constexpr int MAX_BROWSER_ENTRIES = 128;
static String g_browser_entries[MAX_BROWSER_ENTRIES];
static bool g_browser_is_dir[MAX_BROWSER_ENTRIES];
static int g_browser_count = 0;
static int g_browser_cursor = 0;
static String g_browser_path = "/";
static volatile bool g_emulator_ready = false;
static volatile bool g_emulator_init_failed = false;
static volatile bool g_audio_task_started = false;


// frame_buf has been removed to save 32KB SRAM

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

    File dir = SD.open(path.c_str());
    if (!dir || !dir.isDirectory())
        return;

    if (path != "/" && path != "/sd" && path != "/sd/" && g_browser_count < MAX_BROWSER_ENTRIES) {
        g_browser_entries[g_browser_count] = "..";
        g_browser_is_dir[g_browser_count] = true;
        g_browser_count++;
    }

    while (true) {
        File entry = dir.openNextFile();
        if (!entry)
            break;

        String name = entry.name();
        if (name == "." || name == "..") {
            entry.close();
            continue;
        }

        if (entry.isDirectory()) {
            if (g_browser_count < MAX_BROWSER_ENTRIES) {
                g_browser_entries[g_browser_count] = name;
                g_browser_is_dir[g_browser_count] = true;
                g_browser_count++;
            }
        } else if (is_rom_file(name)) {
            if (g_browser_count < MAX_BROWSER_ENTRIES) {
                g_browser_entries[g_browser_count] = name;
                g_browser_is_dir[g_browser_count] = false;
                g_browser_count++;
            }
        }

        entry.close();
    }

    dir.close();
}

static void draw_browser()
{
    uint16_t r4_yellow = M5Cardputer.Display.color565(253, 192, 0);
    M5Cardputer.Display.fillScreen(r4_yellow);
    M5Cardputer.Display.setTextColor(TFT_BLACK, r4_yellow);

    M5Cardputer.Display.setCursor(5, 5);
    M5Cardputer.Display.setTextSize(2); // Make title font larger
    M5Cardputer.Display.println("R8 ROM BROWSER");
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(5, 25);
    M5Cardputer.Display.println(g_browser_path);

    // Draw black border around file list
    M5Cardputer.Display.drawRect(2, 35, 236, 80, TFT_BLACK);
    
    M5Cardputer.Display.setTextSize(2); // Make font larger
    int visible = 5; // Reduced visible items due to larger font
    int start = 0;
    if (g_browser_count > visible) {
        if (g_browser_cursor >= visible)
            start = g_browser_cursor - visible + 1;
        if (start + visible > g_browser_count)
            start = g_browser_count - visible;
    }

    int y_pos = 38;
    for (int i = start; i < start + visible && i < g_browser_count; i++) {
        M5Cardputer.Display.setCursor(6, y_pos);
        M5Cardputer.Display.print(i == g_browser_cursor ? ">" : " ");
        if (g_browser_is_dir[i])
            M5Cardputer.Display.println(String("[" + g_browser_entries[i] + "]").c_str());
        else
            M5Cardputer.Display.println(g_browser_entries[i].c_str());
        y_pos += 16;
    }

    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(5, 120);
    M5Cardputer.Display.println("UP: W/; DOWN: S/. ENTER: Open");
}

static String browse_for_rom()
{
    g_browser_path = "/";
    scan_browser_directory(g_browser_path);
    draw_browser();

    while (true) {
        M5Cardputer.update();
        M5Cardputer.Keyboard.updateKeyList();
        M5Cardputer.Keyboard.updateKeysState();

        auto& ks = M5Cardputer.Keyboard.keysState();

        bool up = false;
        bool down = false;
        bool left = false;
        bool right = false;
        bool enter = ks.enter || ks.space;

        for (char c : ks.word) {
            if (c == 'w' || c == 'W' || c == 'u' || c == 'U' || c == ';') up = true;
            if (c == 's' || c == 'S' || c == '.') down = true;
            if (c == 'a' || c == 'A' || c == ',') left = true;
            if (c == 'd' || c == 'D' || c == '/') right = true;
            if (c == '\n' || c == '\r') enter = true;
        }

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
            if (g_browser_cursor < 0) g_browser_cursor = 0; // Check for 0 elements
            draw_browser();
        } else if (enter && g_browser_count > 0) {
            if (g_browser_is_dir[g_browser_cursor]) {
                if (g_browser_entries[g_browser_cursor] == "..") {
                    int last_slash = g_browser_path.lastIndexOf('/', g_browser_path.length() - 2);
                    if (last_slash >= 0) {
                        g_browser_path = g_browser_path.substring(0, last_slash + 1);
                    } else {
                        g_browser_path = "/";
                    }
                } else {
                    String next = g_browser_path;
                    if (!next.endsWith("/")) next += "/";
                    next += g_browser_entries[g_browser_cursor];
                    g_browser_path = next;
                }
                scan_browser_directory(g_browser_path);
                draw_browser();
            } else {
                return g_browser_path + (g_browser_path.endsWith("/") ? "" : "/") + g_browser_entries[g_browser_cursor];
            }
        }

        delay(50);
    }
}

static void audio_task(void *pvParameters)
{
    const int sample_count = 512;
    int16_t *audio_buf[2];
    audio_buf[0] = (int16_t *)malloc(sample_count * sizeof(int16_t));
    audio_buf[1] = (int16_t *)malloc(sample_count * sizeof(int16_t));

    if (!audio_buf[0] || !audio_buf[1]) {
        if (audio_buf[0]) free(audio_buf[0]);
        if (audio_buf[1]) free(audio_buf[1]);
        vTaskDelete(NULL);
        return;
    }

    int current_buf = 0;

    while (1) {
        render_sounds(audio_buf[current_buf], sample_count);

        while (!M5Cardputer.Speaker.playRaw(audio_buf[current_buf], sample_count, 44100, false, 1, 0)) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        current_buf = 1 - current_buf;
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
        if (c == ' ' || c == 'z' || c == 'Z' || c == 'v' || c == 'V') mask |= 16; // O
        if (c == 0x1b || c == '\t' || c == 'm' || c == 'M') mask |= 64; // START
    }

    m_buttons[0] = mask;
}

extern "C" void m5stack_suspend_frame_buf(void) { }
extern "C" void m5stack_resume_frame_buf(void) { }


extern "C" void p8_render()
{
    if (!m_memory) return;
    uint8_t* vram = &m_memory[0x6000]; // VRAM starts at 0x6000
    
    // Decode 4-bit indexed colors to 16-bit RGB565 into a local line buffer
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
    M5Cardputer.Display.println("RONTO8, creating a nu wave in the fantasy console scene.");
    M5Cardputer.Display.setCursor(10, 75);
    M5Cardputer.Display.println("Powering beyond hardware limitations, high-speed emulation");
    M5Cardputer.Display.setCursor(10, 85);
    M5Cardputer.Display.println("and advanced dynamics accelerate the rush of portable coding.");

    M5Cardputer.Display.setCursor(10, 120);
    M5Cardputer.Display.print("(C)2026 RONTO8. BASED ON FEMTO8 & ZEPTO8.");

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
        M5Cardputer.Display.print("*PRESS ANY KEY");

        delay(50);
    }
    
    // 画面を黒ではなく黄色にクリアし、レターボックス（左右56pxずつ）に装飾を描画
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
}

void setup()
{
    Serial.begin(115200);
    delay(3000); // Wait for serial monitor to connect
    Serial.println("--- M5CARDPUTER BOOT ---");
    
    // Allocate frame_buf and m_memory here when the heap is completely free, 
    // to guarantee we get large contiguous blocks (96KB total).
    // frame_buf allocation removed. m_memory is allocated elsewhere.

    extern unsigned char *m_memory;
    if (!m_memory) {
        m_memory = (uint8_t *)malloc(MEMORY_SIZE); // 32KB
        if (m_memory) {
            memset(m_memory, 0, MEMORY_SIZE);
            Serial.printf("[Memory] m_memory (32KB) allocated as early singleton.\n");
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
    
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setColorDepth(16);
    M5Cardputer.Display.setSwapBytes(true);
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(0, 0);

    delay(500);

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
        
        // Draw letterbox background again just before loading, to clear browser
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

        // Clear the PICO-8 screen area to black with "Loading" text
        M5Cardputer.Display.fillRect(56, 3, 128, 128, TFT_BLACK);
        M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5Cardputer.Display.setCursor(60, 60);
        M5Cardputer.Display.print("Loading...");

        String vfs_path = "/sd" + found_file;

        char* param_path = strdup(vfs_path.c_str());
        g_emulator_ready = false;
        g_emulator_init_failed = false;

        xTaskCreatePinnedToCore(emulator_init_task, "emulator_init_task", 8192, (void *)param_path, 2, NULL, 1);

        return;
    } else {
        M5Cardputer.Display.println("No ROM selected.");
    }

    M5Cardputer.Display.println("\nHalting. Please check SD card.");
    while(1) { delay(100); }
}

void loop() {
    M5Cardputer.update();
    M5Cardputer.Keyboard.updateKeyList();
    M5Cardputer.Keyboard.updateKeysState();

    if (g_emulator_init_failed) {
        static bool shown = false;
        if (!shown) {
            shown = true;
            M5Cardputer.Display.fillScreen(TFT_BLACK);
            M5Cardputer.Display.setCursor(0, 0);
            M5Cardputer.Display.println("ROM init failed.");
            M5Cardputer.Display.println("Please choose another cart.");
        }
    } else if (g_emulator_ready && !g_audio_task_started) {
        xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, NULL, configMAX_PRIORITIES - 1, NULL, 0);
        g_audio_task_started = true;
    } else if (g_emulator_ready) {
        p8_step();
    }

    delay(1);
}
