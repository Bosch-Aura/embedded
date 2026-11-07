#pragma once
#include "sim_core.h"
#include <stdio.h>
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
#define INPUT_PULLUP 2
#define INPUT_PULLDOWN 9
#define SERIAL_8N1 0
#define CHANGE 3
#define ARDUINO_ISR_ATTR
#define digitalPinToInterrupt(p) (p)
void attachInterrupt(int pin, void (*isr)(void), int mode);
uint32_t micros();
void pinMode(int, int);
void digitalWrite(int pin, int v);
int digitalRead(int pin);
void analogWrite(int pin, int v);
int analogRead(int pin);
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
uint32_t millis();
uint32_t pulseIn(int, int, uint32_t);
void esp_restart();
class Stream {
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual ~Stream() {}
};
class FakeSerial : public Stream {
public:
    explicit FakeSerial(const char *tag) : tag_(tag) {}
    void begin(unsigned long, int = 0, int = -1, int = -1) {}
    size_t setTxBufferSize(size_t n) { return n; }
    size_t setRxBufferSize(size_t n) { return n; }
    int available() override;
    int read() override;
    size_t write(const uint8_t *buf, size_t len);
    size_t print(const char *s) { return write((const uint8_t *)s, strlen(s)); }
    size_t println(const char *s) { size_t n = print(s); return n + print("\r\n"); }
    void flush() {}
    void feed(const std::string &bytes);
private:
    const char *tag_;
    std::mutex m_;
    std::deque<char> in_;
    std::string pending_out_;
};
extern FakeSerial Serial;
extern FakeSerial Serial1;
