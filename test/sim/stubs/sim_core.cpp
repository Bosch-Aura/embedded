#include "Arduino.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "hardware.h"
#include "ESP32Servo.h"
#include <vector>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <sstream>
#include <cstdlib>

static const auto t0 = std::chrono::steady_clock::now();
uint32_t sim_millis() {
    // -DSIM_TICK_OFFSET=<ms> starts the clock elsewhere, e.g. just before the
    // 32-bit wrap to test the watchdog there.
#ifndef SIM_TICK_OFFSET
#define SIM_TICK_OFFSET 1u
#endif
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() + (uint32_t)(SIM_TICK_OFFSET);
}
uint32_t micros() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
}
void sim_sleep_ms(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static std::mutex out_m;
void sim_out(const std::string &line) {
    std::lock_guard<std::mutex> g(out_m);
    std::cout << "t=" << sim_millis() << " " << line << std::endl;
}

// ---- tasks ----
struct sim_task { std::mutex m; std::condition_variable cv; uint32_t count = 0; };
static thread_local sim_task *current = nullptr;
TickType_t xTaskGetTickCount(void) { return sim_millis(); }
void vTaskDelay(TickType_t t) { sim_sleep_ms(t); }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current; }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *, uint32_t, void *params,
                                   UBaseType_t, TaskHandle_t *handle, BaseType_t) {
    sim_task *t = new sim_task();
    if (handle) *handle = t;
    std::thread([=] { current = t; fn(params); }).detach();
    sim_sleep_ms(2); // let the task grab its handle, like the scheduler would
    return pdPASS;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait) {
    sim_task *t = current;
    std::unique_lock<std::mutex> l(t->m);
    t->cv.wait_for(l, std::chrono::milliseconds(wait), [&] { return t->count > 0; });
    uint32_t v = t->count;
    if (clear) t->count = 0; else if (v) t->count--;
    return v;
}
void xTaskNotifyGive(TaskHandle_t h) { { std::lock_guard<std::mutex> g(h->m); h->count++; } h->cv.notify_all(); }
BaseType_t xTaskNotify(TaskHandle_t h, uint32_t, eNotifyAction) { xTaskNotifyGive(h); return pdPASS; }
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t) {
    std::lock_guard<std::mutex> g(q->m);
    if (q->q.size() >= q->n) return pdFALSE;
    q->q.emplace_back((const uint8_t *)item, (const uint8_t *)item + q->sz);
    q->cv.notify_all();
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait) {
    std::unique_lock<std::mutex> l(q->m);
    if (!q->cv.wait_for(l, std::chrono::milliseconds(wait), [&] { return !q->q.empty(); })) return pdFALSE;
    memcpy(item, q->q.front().data(), q->sz);
    q->q.pop_front();
    return pdTRUE;
}

// ---- serial ----
FakeSerial Serial("USB");
FakeSerial Serial1("UART");
int FakeSerial::available() { std::lock_guard<std::mutex> g(m_); return (int)in_.size(); }
int FakeSerial::read() { std::lock_guard<std::mutex> g(m_); if (in_.empty()) return -1; char c = in_.front(); in_.pop_front(); return (unsigned char)c; }
void FakeSerial::feed(const std::string &b) { std::lock_guard<std::mutex> g(m_); for (char c : b) in_.push_back(c); }
size_t FakeSerial::write(const uint8_t *buf, size_t len) {
    std::string lines;
    {
        std::lock_guard<std::mutex> g(m_);
        for (size_t i = 0; i < len; i++) {
            char c = (char)buf[i];
            if (c == '\n') { lines += std::string(tag_) + " " + pending_out_ + "\n"; pending_out_.clear(); }
            else if (c != '\r') pending_out_ += c;
        }
    }
    std::istringstream ss(lines); std::string l;
    while (std::getline(ss, l)) sim_out(l);
    return len;
}

// ---- hardware: pin level, the REAL src/hardware.cpp runs on top ----
#include <random>
static std::mutex hw_m;
static int in3 = 0, in4 = 0, pwm = 0, servo = -1;
static int energised = 0;            // last energised direction: 1 F, -1 B, 0 none
static uint32_t pwm_zero_since = 0;
static void hw_report() {
    const char *dir = (in3 && !in4) ? "F" : (!in3 && in4) ? "B" : "BRAKE";
    sim_out(std::string("#HW motor dir=") + dir + " pwm=" + std::to_string(pwm));
}
static void bridge_changed() {
    int now_dir = (in3 && !in4) ? 1 : (!in3 && in4) ? -1 : 0;
    if (now_dir != 0 && energised != 0 && now_dir != energised) {
        if (pwm > 0) sim_out("#HW VIOLATION plugging: direction flipped with pwm=" + std::to_string(pwm));
        else sim_out("#HW reverse deadtime=" + std::to_string(sim_millis() - pwm_zero_since));
    }
    if (now_dir != 0) energised = now_dir;
}
void Servo::write(int a) { std::lock_guard<std::mutex> g(hw_m); if (a == servo) return; servo = a; sim_out("#HW servo=" + std::to_string(a)); }

// Ultrasonic model. Each read takes the next value of `pattern` (cycling);
// 0 = no echo. Then optional dropout (no echo) and +/- noise.
static std::mutex us_m;
static std::vector<int> us_pattern{0};
static size_t us_idx = 0;
static double us_dropout = 0.0;
static int us_noise = 0;
static bool us_log = false;
static std::mt19937 rng(12345);
static bool trig_pulse = false;
static int us_next_cm() {
    std::lock_guard<std::mutex> g(us_m);
    int d = us_pattern[us_idx++ % us_pattern.size()];
    if (d > 0 && us_dropout > 0 && std::uniform_real_distribution<double>(0, 1)(rng) < us_dropout) d = 0;
    if (d > 0 && us_noise > 0) d += std::uniform_int_distribution<int>(-us_noise, us_noise)(rng);
    return d;
}
// Echo edges for the interrupt-driven driver: TRIG HIGH starts a pulse on ECHO
// after ~300 us, lasting d*58 us, or ~38 ms with no echo (HC-SR04 datasheet:
// "nothing in range" is a 38 ms pulse). The registered ISR sees both edges.
static void (*echo_isr_fn)(void) = nullptr;
static std::atomic<int> echo_level{0};
void attachInterrupt(int pin, void (*isr)(void), int) { if (pin == GPIO_ULTRASONIC_ECHO) echo_isr_fn = isr; }
// Sleep most of the way, then spin: a plain sleep wakes 50-100 us late, which
// is 1-2 cm of error in the measured echo.
static void wait_until_us(uint32_t target) {
    int32_t left = (int32_t)(target - micros());
    if (left > 400) std::this_thread::sleep_for(std::chrono::microseconds(left - 300));
    while ((int32_t)(target - micros()) > 0) {}
}
static void echo_pulse_async() {
    int d = us_next_cm();
    const uint32_t trig = micros();
    std::thread([d, trig] {
        const uint32_t rise = trig + 300;
        wait_until_us(rise);
        echo_level = 1; if (echo_isr_fn) echo_isr_fn();
        uint32_t width = d > 0 && d * 58 < 30000 ? (uint32_t)(d * 58) : 38000u;
        wait_until_us(rise + width);
        echo_level = 0; if (echo_isr_fn) echo_isr_fn();
        if (us_log) sim_out(d > 0 && width != 38000u ? "#US raw=" + std::to_string(d) : std::string("#US raw=noecho"));
    }).detach();
}

uint32_t pulseIn(int pin, int, uint32_t timeout_us) {
    if (pin != GPIO_ULTRASONIC_ECHO || !trig_pulse) return 0;
    trig_pulse = false;
    int d = us_next_cm();
    uint32_t us = d > 0 ? (uint32_t)(d * 58) : 0;
    if (d <= 0 || us > timeout_us) {           // no echo: pulseIn waits the whole timeout
        std::this_thread::sleep_for(std::chrono::microseconds(timeout_us));
        if (us_log) sim_out("#US raw=noecho");
        return 0;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(us + 300)); // ~trigger-to-echo delay
    if (us_log) sim_out("#US raw=" + std::to_string(d));
    return us;
}

static std::atomic<bool> sim_estop{false};
void pinMode(int, int) {}
void digitalWrite(int pin, int v) {
    std::lock_guard<std::mutex> g(hw_m);
    if (pin == GPIO_ULTRASONIC_TRIG) {
        if (v) {
            if (echo_isr_fn) echo_pulse_async();   // interrupt driver
            else trig_pulse = true;                // pulseIn() driver
        }
        return;
    }
    if (pin == GPIO_MOTOR_IN3 || pin == GPIO_MOTOR_IN4) {
        int &p = (pin == GPIO_MOTOR_IN3) ? in3 : in4;
        if (p == v) return;
        p = v; bridge_changed(); hw_report();
    }
}
int digitalRead(int pin) {
    if (pin == GPIO_ULTRASONIC_ECHO) return echo_level;
    return (pin == GPIO_ESTOP && sim_estop) ? 0 : 1;
}
void analogWrite(int pin, int v) {
    if (pin != GPIO_MOTOR_ENB) return;
    std::lock_guard<std::mutex> g(hw_m);
    if (v == pwm) return;
    if (v == 0) pwm_zero_since = sim_millis();
    pwm = v; hw_report();
}
int analogRead(int) { return 0; }
void delay(uint32_t ms) { sim_sleep_ms(ms); }
void delayMicroseconds(uint32_t) {}
uint32_t millis() { return sim_millis(); }
void esp_restart() { sim_out("#HW RESTART"); std::exit(3); }

static void sim_command(std::istringstream &ss, const std::string &k) {
    if (k == "dist") { int d; ss >> d; std::lock_guard<std::mutex> g(us_m); us_pattern = {d}; us_idx = 0; }
    else if (k == "pattern") {            // e.g. "#SIM pattern 6,0,0,0"
        std::string list; ss >> list; std::vector<int> v; std::stringstream ls(list); std::string x;
        while (std::getline(ls, x, ',')) v.push_back(std::stoi(x));
        std::lock_guard<std::mutex> g(us_m); us_pattern = v; us_idx = 0;
    }
    else if (k == "dropout") { double p; ss >> p; std::lock_guard<std::mutex> g(us_m); us_dropout = p; }
    else if (k == "noise") { int n; ss >> n; std::lock_guard<std::mutex> g(us_m); us_noise = n; }
    else if (k == "uslog") { int e; ss >> e; us_log = e != 0; }
    else if (k == "estop") { int e; ss >> e; sim_estop = e != 0; }
    else if (k == "uart") { std::string rest; std::getline(ss, rest); Serial1.feed(rest.substr(1) + "\n"); }
    else if (k == "now") sim_out("#SIM now=" + std::to_string(sim_millis()));
    else if (k == "quit") std::exit(0);
}

void setup(void);
void loop(void);
int main() {
    std::thread([] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.rfind("#SIM ", 0) == 0) {
                std::istringstream ss(line.substr(5)); std::string k; ss >> k;
                sim_command(ss, k);
            } else {
                Serial.feed(line + "\n");
            }
        }
        std::exit(0);
    }).detach();
    setup();
    while (true) loop();
}
