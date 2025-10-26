#include <stdlib.h>
#include <stdio.h>
#include "chunk.h"
#include "memory.h"
#include "vm.h"

void initChunk(Chunk* chunk) {
    chunk->capacity = 0;
    chunk->count = 0;
    chunk->lineArray.count = 0;
    chunk->lineArray.capacity = 0;
    chunk->lineArray.lines = NULL;
    chunk->code = NULL;


    initValueArray(&chunk->constants);

}

void writeChunk(Chunk* chunk, uint8_t byte, int line) {
    if (chunk->capacity < chunk->count + 1) {
            int oldCapacity = chunk->capacity;
            chunk->capacity = GROW_CAPACITY(oldCapacity);
            chunk->code = GROW_ARRAY(uint8_t, chunk->code, oldCapacity, chunk->capacity);

        }

    if (chunk->lineArray.count > 0 && chunk->lineArray.lines[chunk->lineArray.count - 1].line == line) {
        chunk->lineArray.lines[chunk->lineArray.count - 1].offsetCount++;
    } else {

        if (chunk->lineArray.count >= chunk->lineArray.capacity) {
            int oldCapacity = chunk->lineArray.capacity;
            chunk->lineArray.capacity = GROW_CAPACITY(oldCapacity);
            chunk->lineArray.lines = GROW_ARRAY(Line, chunk->lineArray.lines, oldCapacity, chunk->lineArray.capacity);
        }

        chunk->lineArray.lines[chunk->lineArray.count].line = line;
        chunk->lineArray.lines[chunk->lineArray.count].offsetCount = 1;
        chunk->lineArray.count++;
    }

    chunk->code[chunk->count++] = byte;
}


void freeChunk(Chunk* chunk) {
    FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
    freeValueArray(&chunk->constants);
    initChunk(chunk);
}

uint32_t addConstant(Chunk* chunk, Value value) {
    push(value);
    writeValueArray(&chunk->constants, value);
    pop();
    return (uint32_t)(chunk->constants.count - 1);
}


void writeConstant(Chunk* chunk, Value value, int line) {

    uint32_t constant = addConstant(chunk, value);

    if (constant < 256) {
        writeChunk(chunk, OP_CONSTANT, line);
        writeChunk(chunk, constant, line);

    } else {
        writeChunk(chunk, OP_CONSTANT_LONG, line);
        writeChunk(chunk, (uint8_t)((constant & 0x000000ff)), line);
        writeChunk(chunk, (uint8_t)((constant & 0x0000ff00) >> 8), line);
        writeChunk(chunk, (uint8_t)((constant & 0x00ff0000) >> 16), line);
    }
}

