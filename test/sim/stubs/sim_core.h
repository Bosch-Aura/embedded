// Host simulation core: real threads, real (ms) time.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <mutex>
#include <condition_variable>
#include <string>
#include <deque>
uint32_t sim_millis();
void sim_sleep_ms(uint32_t ms);
void sim_out(const std::string &line); // to stdout, atomic
