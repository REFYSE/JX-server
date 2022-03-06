#ifndef BINIO_H
#define BINIO_H
#include <stdio.h>
#include <stdint.h>

size_t fread_b(uint8_t* ptr, size_t nbits, FILE* file);
uint64_t read_b(uint64_t src, size_t index, size_t len);
size_t write_b(uint8_t* dest, size_t index, size_t len, uint8_t* val);
#endif
