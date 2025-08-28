#ifndef clox_compiler_h
#define clox_compiler_h
#include "vm.h"
#include "clox_scanner.h"

typedef struct {
    Token previous;
    Token current;
    bool hadError;
    bool panicMode;
} Parser;

 typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,  
    PREC_OR,          
    PREC_AND,         
    PREC_EQUALITY,    
    PREC_COMPARISON, 
    PREC_TERM,        
    PREC_FACTOR,      
    PREC_UNARY,       
    PREC_CALL,        
    PREC_PRIMARY
 } Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct {
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

typedef struct {
    Token name;
    int depth;
    bool isConst;
} Local;

typedef struct {
    Local locals[UINT8_COUNT];
    int localCount;
    int scopeDepth;

    Table constGlobals;
} Compiler;

typedef struct {
    int breakJumps[UINT8_MAX];
    int breakCount;
    int depth;
} BreakEntries;


bool compile(const char* source, Chunk* chunk);

#endif