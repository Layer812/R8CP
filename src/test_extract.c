#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

int parse_cart_file(const char* file_name, uint8_t* memory, const char** lua_script, uint8_t** file_buffer, uint8_t* label_image);

// Stubs for pngle / etc to link if needed? No, I'll just compile it inside src/ directory context
// Wait, compiling might be too annoying.
