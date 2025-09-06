#ifndef clox_chunk_h
#define clox_chunk_h
#include "common.h"
#include "value.h"


typedef enum {
    OP_CONSTANT,
    OP_CONSTANT_LONG,
    OP_NIL,
    OP_TRUE,
    OP_FALSE,
    OP_POP,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_LOOP,
    OP_CONST,
    OP_GET_LOCAL,
    OP_GET_GLOBAL,
    OP_GET_GLOBAL_LONG,
    OP_DEFINE_GLOBAL,
    OP_DEFINE_CONST_GLOBAL,
    OP_DEFINE_GLOBAL_LONG,
    OP_DEFINE_CONST_GLOBAL_LONG,
    OP_SET_LOCAL,
    OP_SET_GLOBAL,
    OP_SET_GLOBAL_LONG,
    OP_CALL,
    OP_ARRAY,
    OP_ARRAY_LONG,
    OP_GET_ARRAY_ELEMENT,
    OP_SET_ARRAY_ELEMENT,
    OP_GET_ARRAY_GLOBAL_ELEMENT,
    OP_SET_ARRAY_GLOBAL_ELEMENT,
    OP_GET_ARRAY_GLOBAL_ELEMENT_LONG,
    OP_SET_ARRAY_GLOBAL_ELEMENT_LONG,
    OP_EQUAL,
    OP_GREATER,
    OP_LESS,
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_NOT,
    OP_NEGATE,
    OP_PRINT,
    OP_RETURN,
} OpCode;


typedef struct {
    int line;
    int offsetCount;
} Line;

typedef struct {
    int capacity;
    int count;
    Line* lines;
} LineArray;
typedef struct {
    int count;
    int capacity;
    uint8_t* code;
    LineArray lineArray;
    ValueArray constants;
} Chunk;




void initChunk(Chunk* chunk);
void freeChunk(Chunk* chunk);
void writeChunk(Chunk* chunk, uint8_t byte, int line);
uint32_t addConstant(Chunk* chunk, Value value);
void writeConstant(Chunk* chunk, Value value, int line);

#endif
