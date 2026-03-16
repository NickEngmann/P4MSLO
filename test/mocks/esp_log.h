#ifndef ESP_LOG_H
#define ESP_LOG_H

#include <stdio.h>

#define ESP_LOGI(tag, fmt, ...) printf("%s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("%s: ERROR - " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("%s: WARN - " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) printf("%s: DEBUG - " fmt "\n", tag, ##__VA_ARGS__)

#endif /* ESP_LOG_H */
