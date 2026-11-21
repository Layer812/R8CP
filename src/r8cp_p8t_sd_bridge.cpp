#include <Arduino.h>
#include <SD.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static const char *sd_relative_path(const char *path) {
    if (!path) return NULL;
    if (strcmp(path, "/sd") == 0) return "/";
    if (strncmp(path, "/sd/", 4) == 0) return path + 3;
    if (path[0] == '/') return path;
    return NULL;
}

extern "C" long r8cp_p8t_sd_size(const char *path) {
    const char *relative = sd_relative_path(path);
    if (!relative) return -1;
    File file = SD.open(relative, FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) file.close();
        return -1;
    }
    const size_t n = file.size();
    file.close();
    if (n == 0 || n > LONG_MAX) return -1;
    return (long)n;
}

extern "C" long r8cp_p8t_sd_read(const char *path, char *buffer, size_t length) {
    const char *relative = sd_relative_path(path);
    if (!relative || !buffer) return -1;
    File file = SD.open(relative, FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) file.close();
        return -1;
    }
    if (file.size() != length) {
        file.close();
        return -1;
    }
    size_t pos = 0;
    while (pos < length) {
        const int got = file.read((uint8_t *)buffer + pos, length - pos);
        if (got <= 0) { file.close(); return -1; }
        pos += (size_t)got;
    }
    file.close();
    return (long)pos;
}
