#pragma once
using gpio_num_t = int;
using gpio_pull_mode_t = int;
enum {GPIO_MODE_INPUT,GPIO_MODE_INPUT_OUTPUT,GPIO_PULLDOWN_ONLY,GPIO_PULLUP_ONLY,GPIO_FLOATING};
struct MockGpioTrace { int directions[40] = {}; int pulls[40] = {}; };
extern MockGpioTrace mockGpioTrace;
inline int gpio_get_level(int) { return 0; }
inline void gpio_set_direction(int pin,int direction) { mockGpioTrace.directions[pin]=direction; }
inline void gpio_set_pull_mode(int pin,int pull) { mockGpioTrace.pulls[pin]=pull; }
inline void gpio_set_level(int,int) {}
