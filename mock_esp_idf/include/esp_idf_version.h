#ifndef ESP_IDF_VERSION_H
#define ESP_IDF_VERSION_H

#define ESP_IDF_VERSION_VAL(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))

#ifndef ESP_IDF_VERSION
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5, 1, 0)
#endif

#endif /* ESP_IDF_VERSION_H */

