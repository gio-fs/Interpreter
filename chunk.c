#include <stdlib.h>
#include "chunk.h"
#include "memory.h"

void initChunk(Chunk* chunk) {

    chunk->capacity = 0;
    chunk->count = 0;
    chunk->linesLength = 0;
    chunk->linesCapacity = 0;
    chunk->lines = NULL;
    chunk->code = NULL;
    initValueArray(&chunk->constants);

}

void writeChunk(Chunk* chunk, uint8_t byte, int line) {

    if (chunk->capacity < chunk->count+1) {
            int oldCapacity = chunk->capacity;
            chunk->capacity = GROW_CAPACITY(oldCapacity);
            chunk->code = GROW_ARRAY(uint8_t, chunk->code, oldCapacity, chunk->capacity);

        }

    if (chunk->linesLength > 0 && chunk->lines[chunk->linesLength - 1].line == line) {
        chunk->lines[chunk->linesLength-1].offsetCount++;
    } else {

        if (chunk->linesLength >= chunk->linesCapacity) {
            int oldCapacity = chunk->linesCapacity;
            chunk->linesCapacity = GROW_CAPACITY(oldCapacity);
            chunk->lines = GROW_ARRAY(Line, chunk->lines, oldCapacity, chunk->linesCapacity);
        }

        chunk->lines[chunk->linesLength].line = line;
        chunk->lines[chunk->linesLength].offsetCount = 1;
        chunk->linesLength++;
    }

    chunk->code[chunk->count] = byte;
    chunk->count++;
}


void freeChunk(Chunk* chunk) {

    FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
    freeValueArray(&chunk->constants);
    initChunk(chunk);
}

uint32_t addConstant(Chunk* chunk, Value value) {
    writeValueArray(&chunk->constants, value);
    return (uint32_t)(chunk->constants.count - 1);
}


void writeConstant(Chunk* chunk, Value value, int line) {

    uint32_t constant = addConstant(chunk, value);

    if (constant < 256) {

        writeChunk(chunk, OP_CONSTANT, line);
        writeChunk(chunk, constant, line);

    }

    else {

        writeChunk(chunk, OP_CONSTANT_LONG, line);
        writeChunk(chunk, (uint8_t)((constant & 0x000000ff)), line);
        writeChunk(chunk, (uint8_t)((constant & 0x0000ff00) >> 8), line);
        writeChunk(chunk, (uint8_t)((constant & 0x00ff0000) >> 16), line);

    }
}

