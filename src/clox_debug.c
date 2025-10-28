#include <stdio.h>

#include "clox_debug.h"
#include "value.h"
#include "object.h"

void disassembleChunk(Chunk* chunk, const char* name) {
    printf("== %s ==\n", name);

    for (int offset = 0; offset < chunk->count;) {
        offset = disassembleInstruction(chunk, offset);
    }
}

static int simpleInstruction(const char* name, int offset) {
    printf("%s\n", name);
    return offset + 1;
}
static int byteInstruction(const char* name, Chunk* chunk, int offset) {
    uint8_t slot = chunk->code[offset + 1];
    printf("%-16s %4d\n", name, slot);
    return offset + 2;
}

static int constantInstruction(const char* name, Chunk* chunk, int offset) {
    uint8_t constant = chunk->code[offset+1];
    printf("%-16s %4d '", name, constant);
    printValue(chunk->constants.values[constant]);
    printf("' \n");
    return offset + 2;
}

static int constantLongInstruction(const char* name, Chunk* chunk, int offset) {
    uint32_t constant = (chunk->code[offset + 1]) |
                        (chunk->code[offset + 2] << 8) |
                        (chunk->code[offset + 3] << 16);

    printf("%-16s %4d '", name, constant);
    printValue(chunk->constants.values[constant]);
    printf("' \n");
    return offset + 4;
}

static int jumpInstruction(const char* name, int sign, Chunk* chunk, int offset) {
    uint16_t jump = (uint16_t)(chunk->code[offset + 1] << 8);
    jump |= chunk->code[offset + 2];
    printf("%-16s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
    return offset + 3;
}

int getLine(Chunk* chunk, int index) {

    int count = 0;

    for(int i = 0; i < chunk->lineArray.count; i++) {

        count += chunk->lineArray.lines[i].offsetCount;

        if (index < count) {
            return chunk->lineArray.lines[i].line;
        }

    }

    //error
    return -1;
}

int disassembleInstruction(Chunk* chunk, int offset) {

    printf("%04d ", offset);

    int line = getLine(chunk, offset);
    printf("%4d    ", line);

    uint8_t instruction = chunk->code[offset];
    switch (instruction) {
        case OP_CONSTANT:
            return constantInstruction("OP_CONSTANT", chunk, offset);
        case OP_CONSTANT_LONG:
            return constantLongInstruction("OP_CONSTANT_LONG", chunk, offset);
        case OP_NIL:
            return simpleInstruction("OP_NIL", offset);
        case OP_NOT:
            return simpleInstruction("OP_NOT", offset);
        case OP_TRUE:
            return simpleInstruction("OP_TRUE", offset);
        case OP_FALSE:
            return simpleInstruction("OP_FALSE", offset);
        case OP_POP:
            return simpleInstruction("OP_POP", offset);
        case OP_JUMP:
            return jumpInstruction("OP_JUMP", 1, chunk, offset);
        case OP_JUMP_IF_FALSE:
            return jumpInstruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
        case OP_LOOP:
            return jumpInstruction("OP_LOOP", -1, chunk, offset);
        case OP_GET_LOCAL:
            return byteInstruction("OP_GET_LOCAL", chunk, offset);
        case OP_GET_GLOBAL:
            return constantInstruction("OP_GET_GLOBAL", chunk, offset);
        case OP_GET_GLOBAL_LONG:
            return constantLongInstruction("OP_GET_GLOBAL_LONG", chunk, offset);
        case OP_DEFINE_CONST_GLOBAL:
            return constantInstruction("OP_DEFINE_CONST_GLOBAL", chunk, offset);
        case OP_DEFINE_GLOBAL:
            return constantInstruction("OP_DEFINE_GLOBAL", chunk, offset);
        case OP_DEFINE_CONST_GLOBAL_LONG:
            return constantLongInstruction("OP_DEFINE_CONST_GLOBAL", chunk, offset);
        case OP_DEFINE_GLOBAL_LONG:
            return constantLongInstruction("OP_DEFINE_GLOBAL_LONG", chunk, offset);
        case OP_ARRAY:
            return byteInstruction("OP_ARRAY", chunk, offset);
        case OP_ARRAY_LONG:
            return constantLongInstruction("OP_ARRAY_LONG", chunk, offset);
        case OP_MAP:
            return byteInstruction("OP_MAP", chunk, offset);
        case OP_MAP_LONG:
            return constantLongInstruction("OP_MAP_LONG", chunk, offset);
        case OP_GET_ELEMENT:
            return byteInstruction("OP_GET_ELEMENT", chunk, offset);
        case OP_SET_ELEMENT:
            return byteInstruction("OP_SET_ELEMENT", chunk, offset);
        case OP_GET_ELEMENT_GLOBAL:
            return byteInstruction("OP_GET_ELEMENT_GLOBAL", chunk, offset);
        case OP_SET_ELEMENT_GLOBAL:
            return byteInstruction("OP_SET_ELEMENT_GLOBAL", chunk, offset);
        case OP_GET_UPVALUE:
            return byteInstruction("OP_GET_UPVALUE", chunk, offset);
        case OP_SET_UPVALUE:
            return byteInstruction("OP_SET_UPVALUE", chunk, offset);
        case OP_GET_ELEMENT_UPVALUE:
            return byteInstruction("OP_GET_ELEMENT_UPVALUE", chunk, offset);
        case OP_SET_ELEMENT_UPVALUE:
            return byteInstruction("OP_SET_ELEMENT_UPVALUE", chunk, offset);
        case OP_CLOSE_UPVALUE:
            return simpleInstruction("OP_CLOSE_UPVALUE", offset);
        case OP_CLOSURE: {
            offset++;
            uint8_t constant = chunk->code[offset++];
            printf("%-16s %4d ", "OP_CLOSURE", constant);
            printValue(chunk->constants.values[constant]);
            printf("\n");

            ObjFunction* function = AS_FUNCTION(chunk->constants.values[constant]);
            for (int j = 0; j < function->upvalueCount; j++) {
                int isLocal = chunk->code[offset++];
                int index = chunk->code[offset++];
                printf("%04d      |                     %s %d\n",
                        offset - 2, isLocal ? "local" : "upvalue", index);
            }
            return offset;
        }
        case OP_CLOSURE_LONG: {
            offset++;
            uint32_t constant = (chunk->code[offset + 1]) |
                                (chunk->code[offset + 2] << 8) |
                                (chunk->code[offset + 3] << 16);

            printf("%-16s %4d ", "OP_CLOSURE_LONG", constant);
            printValue(chunk->constants.values[constant]);
            printf("\n");

            ObjFunction* function = AS_FUNCTION(chunk->constants.values[constant]);
            for (int j = 0; j < function->upvalueCount; j++) {
                int isLocal = chunk->code[offset++];
                int index = chunk->code[offset++];
                printf("%04d      |                     %s %d\n",
                        offset - 2, isLocal ? "local" : "upvalue", index);
            }
            return offset;
        }
        case OP_FOR_EACH: {
            return simpleInstruction("OP_FOR_EACH", offset);
        }
        case OP_SWAP: {
            uint8_t slot1 = chunk->code[offset + 1];
            uint8_t slot2 = chunk->code[offset + 2];
            printf("%-16s %4d, %4d\n", "OP_SWAP", slot1, slot2);
            return offset + 3;
        }
        case OP_ARRAY_CALL:
            return simpleInstruction("OP_ARRAY_CALL", offset);
        case OP_CALL:
            return byteInstruction("OP_CALL", chunk, offset);
        case OP_SET_LOCAL:
            return byteInstruction("OP_SET_LOCAL", chunk, offset);
        case OP_SET_GLOBAL:
            return constantInstruction("OP_SET_GLOBAL", chunk, offset);
        case OP_SET_GLOBAL_LONG:
            return constantLongInstruction("OP_SET_GLOBAL_LONG", chunk, offset);
        case OP_EQUAL:
            return simpleInstruction("OP_EQUAL", offset);
        case OP_GREATER:
            return simpleInstruction("OP_GREATER", offset);
        case OP_LESS:
            return simpleInstruction("OP_LESS", offset);
        case OP_ADD:
            return simpleInstruction("OP_ADD", offset);
        case OP_SUBTRACT:
            return simpleInstruction("OP_SUBTRACT", offset);
        case OP_MULTIPLY:
            return simpleInstruction("OP_MULTIPLY", offset);
        case OP_DIVIDE:
            return simpleInstruction("OP_DIVIDE", offset);
        case OP_NEGATE:
            return simpleInstruction("OP_NEGATE", offset);
        case OP_PRINT:
            return simpleInstruction("OP_PRINT", offset);
        case OP_RETURN:
            return simpleInstruction("OP_RETURN", offset);
        case OP_PUSH:
            return simpleInstruction("OP_PUSH", offset);
        case OP_INCREMENT_NESTING_LVL:
            return simpleInstruction("OP_INCREMENT_NESTING_LVL", offset);
        case OP_DECREMENT_NESTING_LVL:
            return simpleInstruction("OP_DECREMENT_NESTING_LVL", offset);
        case OP_QUEUE:
            return simpleInstruction("OP_QUEUE", offset);
        case OP_GET_ELEMENT_FROM_TOP:
            return simpleInstruction("OP_GET_ELEMENT_FROM_TOP", offset);
        case OP_QUEUE_CLEAR:
            return simpleInstruction("OP_QUEUE_CLEAR", offset);
        case OP_QUEUE_REWIND:
            return simpleInstruction("OP_QUEUE_REWIND", offset);
        case OP_INDIRECT_STORE:
            return simpleInstruction("OP_INDIRECT_STORE", offset);
        case OP_SAVE_VALUE:
            return simpleInstruction("OP_SAVE_VALUE", offset);
        case OP_REVERSE_N:
            return byteInstruction("OP_REVERSE_N", chunk, offset);
        case OP_PUSH_FROM:
            return byteInstruction("OP_PUSH_FROM", chunk, offset);
        case OP_CHECK_TYPE:
            return byteInstruction("OP_CHECK_TYPE", chunk, offset);
        case OP_RANGE:
            return simpleInstruction("OP_RANGE", offset);
        case OP_DEQUE:
            return simpleInstruction("OP_DEQUE", offset);
        case OP_METHOD:
            return constantInstruction("OP_METHOD", chunk, offset);
        case OP_CLASS:
            return constantInstruction("OP_CLASS", chunk, offset);
        case OP_GET_PROPERTY:
            return constantInstruction("OP_GET_PROPERTY", chunk, offset);
        case OP_SET_PROPERTY:
            return constantInstruction("OP_SET_PROPERTY", chunk, offset);
        case OP_DEFINE_PROPERTY:
            return simpleInstruction("OP_DEFINE_PROPERTY", offset);
        case OP_INVOKE: {
            uint8_t constant = chunk->code[offset + 1];
            uint8_t argCount = chunk->code[offset + 2];
            printf("%-16s (%d args) %4d '", "OP_INVOKE", argCount, constant);
            printValue(chunk->constants.values[constant]);
            printf("'\n");
            return offset + 3;
        }
        case OP_INHERIT:
            return simpleInstruction("OP_INHERIT", offset);
        case OP_GET_SUPER:
            return constantInstruction("OP_GET_SUPER", chunk, offset);
        default:
            printf("Unknown opcode %d\n", instruction);
            return offset + 1;
    }
}



