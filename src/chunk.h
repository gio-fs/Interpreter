#ifndef clox_chunk_h
#define clox_chunk_h
#include "common.h"
#include "value.h"
#include "table.h"

typedef enum {
    OP_CONSTANT,
    OP_CONSTANT_LONG,
    OP_NIL,
    OP_TRUE,
    OP_FALSE,
    OP_POP,
    OP_PUSH,
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
    OP_CLOSURE,
    OP_CALL,
    OP_ARRAY_CALL,
    OP_GET_UPVALUE,
    OP_SET_UPVALUE,
    OP_GET_ELEMENT_UPVALUE,
    OP_SET_ELEMENT_UPVALUE,
    OP_GET_ELEMENT_FROM_TOP,
    OP_SWAP,
    OP_CLOSE_UPVALUE,
    OP_ARRAY,
    OP_ARRAY_LONG,
    OP_MAP,
    OP_MAP_LONG,
    OP_GET_ELEMENT,
    OP_SET_ELEMENT,
    OP_GET_MAP,
    OP_SET_MAP,
    OP_GET_ELEMENT_GLOBAL,
    OP_SET_ELEMENT_GLOBAL,
    OP_GET_MAP_GLOBAL,
    OP_SET_MAP_GLOBAL,
    OP_GET_ELEMENT_GLOBAL_LONG,
    OP_SET_ELEMENT_GLOBAL_LONG,
    OP_GET_MAP_GLOBAL_LONG,
    OP_SET_MAP_GLOBAL_LONG,
    OP_FOR_EACH,
    OP_FOR_EACH_GLOBAL,
    OP_SAVE_VALUE,
    OP_REVERSE_N,
    OP_QUEUE,
    OP_DEQUE,
    OP_QUEUE_REWIND,
    OP_QUEUE_ADVANCE,
    OP_QUEUE_CLEAR,
    OP_INCREMENT_NESTING_LVL,
    OP_DECREMENT_NESTING_LVL,
    OP_CHECK_TYPE,
    OP_INDIRECT_STORE,
    OP_PUSH_FROM,
    OP_RANGE,
    OP_EQUAL,
    OP_EQUAL_AND,
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
    OP_CLASS,
    OP_DEFINE_PROPERTY,
    OP_GET_PROPERTY,
    OP_SET_PROPERTY,
    OP_METHOD,
    OP_INVOKE,
    OP_INHERIT,
    OP_GET_SUPER
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
    Table saved;
} Chunk;




void initChunk(Chunk* chunk);
void freeChunk(Chunk* chunk);
void writeChunk(Chunk* chunk, uint8_t byte, int line);
uint32_t addConstant(Chunk* chunk, Value value);
void writeConstant(Chunk* chunk, Value value, int line);

#endif
