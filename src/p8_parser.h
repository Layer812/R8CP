/*
 * p8_parser.h
 *
 *  Created on: Dec 13, 2023
 *      Author: bbaker
 */

#ifndef P8_PARSER_H
#define P8_PARSER_H

#include <stdint.h>

int parse_cart_ram(uint8_t *buffer, int size, uint8_t *memory, const char **lua_script, uint8_t **decompression_buffer, uint8_t *label_image);
int parse_cart_file(const char *file_name, uint8_t *memory, const char **lua_script, uint8_t **file_buffer, uint8_t *label_image);
void p8_unmap_lua_script(void);
const void* p8_map_cart_memory(const uint8_t* cart_data, size_t length);
void p8_unmap_cart_memory(void);
void convert_utf8_to_p8scii(uint8_t *buffer, size_t len);

#endif
