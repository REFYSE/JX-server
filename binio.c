#include <stdint.h>
#include "binio.h"

// Creates a read bit mask with length n starting at index i
#define GET_RMASK(i, n) (((1 << (n)) - 1) << (i))
// Creates a write bit mask with length n starting at index i
#define GET_WMASK(i, n) (~GET_RMASK((i),(n)))
// Get n bits starting at index i where index 0 is the least significant bit
#define READ_BITS(a, i, n) (((a) >> (i)) & (GET_RMASK(0, (n))))
// Writes n bits of v to a starting at index i
#define WRITE_BITS(a, i, n, v) ((a)=(((a) & (GET_WMASK((i),(n))))|((v) << (i))))

/*
 * Reads nbits number of bits from file into ptr
 * Returns number of bits read
 */
size_t fread_b(uint8_t* ptr, size_t nbits, FILE* file) {
    static uint8_t buffer = 0;
    static uint8_t bcur = 8;
    size_t nwritten = 0;
    uint8_t next_byte = 0;
    int index = 0;
    while(nwritten < nbits) {
        // Read next byte if buffer cursor is pointing to end of buffer
        if(bcur == 8) {
            if(fread(&buffer, 1, 1, file) != 1) {
                return nwritten; // An error occurred
            }
            bcur = 0;
        }
        // Build the next byte to be written
        next_byte = 0;
        // If more than a byte left to be written, build a full byte
        uint8_t remaining = nbits - nwritten;
        if(remaining >= 8) {
            // If a full byte is unused in the buffer, just copy that byte
            if(bcur == 0) {
                next_byte = buffer;
                bcur = 8;
            // Otherwise copy remaining bits from buffer then read next byte...
            // ...and copy bits until a full byte is filled
            } else {
                WRITE_BITS(next_byte, bcur, (8 - bcur), buffer);
                if(fread(&buffer, 1, 1, file) != 1) {
                    return nwritten; // An error occurred
                }
                uint8_t temp = READ_BITS(buffer, (8 - bcur), bcur);
                WRITE_BITS(next_byte, 0, bcur, temp);
            }
            nwritten += 8;
        // Otherwise if less than a full byte left to be written
        } else {
            // If enough bits left in buffer, copy from buffer
            if(8 - bcur >= remaining) {
                uint8_t temp = READ_BITS(buffer, (8 - (bcur + remaining)),
                                            remaining);
                WRITE_BITS(next_byte, (8 - remaining), remaining, temp);
                bcur += remaining;
            // Otherwise copy remaining bits from buffer then read next byte...
            // ...and copy bits until remaining bits are written
            } else {
                WRITE_BITS(next_byte, bcur, (8 - bcur), buffer);
                uint8_t tmp_remain = remaining - (8 - bcur);
                if(fread(&buffer, 1, 1, file) != 1) {
                    return nwritten; // An error occurred
                }
                uint8_t temp = READ_BITS(buffer, (8 - tmp_remain), tmp_remain);
                WRITE_BITS(next_byte, (8 - remaining), tmp_remain, temp);
                bcur = tmp_remain;
            }
            nwritten += remaining;
        }
        // Copy next byte into the ptr
        ptr[index] = next_byte;
        index = nwritten/8;
    }
    return nwritten;
}

/*
 * Returns len bits starting at index of src
 */
uint64_t read_b(uint64_t src, size_t index, size_t len) {
    return (uint64_t) READ_BITS(src, index, len);
}

/*
 * Writes len bits from val into dest starting at index
 * Returns number of bits written 
 */
size_t write_b(uint8_t* dest, size_t index, size_t len, uint8_t* val) {
    size_t n_written = 0;
    uint8_t cur_dest_b;
    uint8_t cur_val_b;
    uint8_t n_bit;
    // Copy bit by bit into the destination
    for(size_t i = 0; i < len; i++) {
        cur_dest_b = dest[(int)((index + n_written)/8)];
        cur_val_b = val[(int)(n_written/8)];
        n_bit = READ_BITS(cur_val_b, (7-(n_written % 8)), 1);
        WRITE_BITS(cur_dest_b, (7-((n_written + index)%8)), 1, n_bit);
        dest[(int)((index + n_written)/8)] = cur_dest_b;
        n_written++;
    }
    return n_written;
}
