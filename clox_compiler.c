#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "object.h"
#include "chunk.h"
#include "clox_debug.h"
#include "clox_compiler.h"
#include "clox_scanner.h"

#define NOT_IN_LOOP -1

Parser parser;

Compiler* current = NULL;

Chunk* compilingChunk;

static void grouping(bool canAssign);
static void unary(bool canAssign);
static void binary(bool canAssign);
static void number(bool canAssign);
static void literal(bool canAssign);
static void string(bool canAssign);
static void variable(bool canAssign);
static void and_(bool canAssign);
static void or_(bool canAssign);

ParseRule rules[] = {
    [TOKEN_LEFT_PAREN]    = {grouping,  NULL,       PREC_NONE},
    [TOKEN_RIGHT_PAREN]   = {NULL,      NULL,       PREC_NONE},
    [TOKEN_LEFT_BRACE]    = {NULL,      NULL,       PREC_NONE}, 
    [TOKEN_RIGHT_BRACE]   = {NULL,      NULL,       PREC_NONE},
    [TOKEN_COMMA]         = {NULL,      NULL,       PREC_NONE},
    [TOKEN_DOT]           = {NULL,      NULL,       PREC_NONE},
    [TOKEN_MINUS]         = {unary,     binary,     PREC_TERM},
    [TOKEN_PLUS]          = {NULL,      binary,     PREC_TERM},
    [TOKEN_SEMICOLON]     = {NULL,      NULL,       PREC_NONE},
    [TOKEN_SLASH]         = {NULL,      binary,     PREC_FACTOR},
    [TOKEN_STAR]          = {NULL,      binary,     PREC_FACTOR},
    [TOKEN_BANG]          = {unary,     NULL,       PREC_NONE},
    [TOKEN_BANG_EQUAL]    = {NULL,      binary,     PREC_EQUALITY},
    [TOKEN_EQUAL]         = {NULL,      NULL,       PREC_NONE},
    [TOKEN_EQUAL_EQUAL]   = {NULL,      binary,     PREC_EQUALITY},
    [TOKEN_GREATER]       = {NULL,      binary,     PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL] = {NULL,      binary,     PREC_COMPARISON},
    [TOKEN_LESS]          = {NULL,      binary,     PREC_COMPARISON},
    [TOKEN_LESS_EQUAL]    = {NULL,      binary,     PREC_COMPARISON},
    [TOKEN_IDENTIFIER]    = {variable,  NULL,       PREC_NONE},
    [TOKEN_STRING]        = {string,    NULL,       PREC_NONE},
    [TOKEN_NUMBER]        = {number,    NULL,       PREC_NONE},
    [TOKEN_AND]           = {NULL,      and_,       PREC_AND},
    [TOKEN_CLASS]         = {NULL,      NULL,       PREC_NONE},
    [TOKEN_ELSE]          = {NULL,      NULL,       PREC_NONE},
    [TOKEN_FALSE]         = {literal,   NULL,       PREC_NONE},         
    [TOKEN_FOR]           = {NULL,      NULL,       PREC_NONE},
    [TOKEN_FUNC]          = {NULL,      NULL,       PREC_NONE},
    [TOKEN_IF]            = {NULL,      NULL,       PREC_NONE},
    [TOKEN_NIL]           = {literal,   NULL,       PREC_NONE},
    [TOKEN_OR]            = {NULL,      or_,        PREC_OR},
    [TOKEN_PRINT]         = {NULL,      NULL,       PREC_NONE},
    [TOKEN_RETURN]        = {NULL,      NULL,       PREC_NONE},
    [TOKEN_SUPER]         = {NULL,      NULL,       PREC_NONE},
    [TOKEN_THIS]          = {NULL,      NULL,       PREC_NONE},
    [TOKEN_TRUE]          = {literal,   NULL,       PREC_NONE},
    [TOKEN_VAR]           = {NULL,      NULL,       PREC_NONE},
    [TOKEN_WHILE]         = {NULL,      NULL,       PREC_NONE},
    [TOKEN_CONST]         = {NULL,      NULL,       PREC_NONE},
    [TOKEN_ERROR]         = {NULL,      NULL,       PREC_NONE},
    [TOKEN_EOF]           = {NULL,      NULL,       PREC_NONE},
};



static Chunk* currentChunk() {
    return compilingChunk;
}

static void errorAt(Token* token, const char* message) {

    if (parser.panicMode) return; 
    parser.panicMode = true;

    fprintf(stderr, "[line %d] Error ", token->line);

    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type == TOKEN_ERROR) {

    } else {
        fprintf(stderr, "at '%.*s'\n", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
    parser.hadError = true;
}

static void error(const char* message) {
    errorAt(&parser.previous, message);
}

static void errorAtCurrent(const char* message) {
    errorAt(&parser.current, message);
}



static void advance() {
    parser.previous = parser.current;

    for (;;) {
        parser.current = scanToken();
        if (parser.current.type != TOKEN_ERROR) break;
        
        errorAtCurrent(parser.current.start);
    }
}

static void consume(TokenType type, const char* message) {
    if (parser.current.type == type) {
        advance();
        return;
    }

    errorAtCurrent(message);
}

static bool checkType(TokenType type) {
    return parser.current.type == type;
}

static bool checkMatch(TokenType type) {
    if (!checkType(type)) return false;
    advance();
    return true;
}


static uint32_t makeConstant(Value value) {
    uint32_t constant = addConstant(currentChunk(), value);
    return constant;
}

static uint32_t identifierConstant(Token* name) {
    return makeConstant(OBJ_VAL(copyString(name->start, name->length)));
}

static bool identifiersEqual(Token* a, Token* b) {
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, b->length) == 0;
}

static int resolveLocal(Compiler* compiler, Token* name) {
    //Locals are resolved from the top
    for (int i = compiler->localCount - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (identifiersEqual(name, &local->name)) {
            if (local->depth == -1) {
                error("Can't read local variable in its own initializer.");
            }
            return i;
        }
    }
    
    return -1;
}

static void emitByte(uint8_t byte) {
    writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2) {
    emitByte(byte1);
    emitByte(byte2);
}

static void emitFourBytes(uint8_t byte1, uint8_t byte2, uint8_t byte3, uint8_t byte4) {
    emitByte(byte1);
    emitByte(byte2);
    emitByte(byte3);
    emitByte(byte4);
}

static void emitConstant(Value value) {
    
    if (currentChunk()->constants.count < UINT8_MAX) {
        uint8_t constant = makeConstant(value);
        emitBytes(OP_CONSTANT, constant);
        
    } else {
        uint32_t constantLong = makeConstant(value);
        emitFourBytes(OP_CONSTANT_LONG, (uint8_t)((constantLong & 0x000000ff)), 
                                        (uint8_t)((constantLong & 0x0000ff00) >> 8), 
                                        (uint8_t)((constantLong & 0x00ff0000) >> 16));

    }
}

static int emitJump(uint8_t instruction) {
    emitByte(instruction);
    emitByte(0xff);
    emitByte(0xff);

    return currentChunk()->count - 2;
}

static void patchJump(int offset) {
    //diff between the end of the then block and the byte before the OP_JUMP_IF_FALSE
    int jump = currentChunk()->count - offset - 2;

    if (jump > UINT16_MAX) {
        error("Jump is larger than 16 bits.");
    }

    //inserting the actual jump value as operand of OP_JUMP_IF_FALSE
    currentChunk()->code[offset] = (jump >> 8) & 0xff;
    currentChunk()->code[offset + 1] = jump & 0xff;
}

static void emitLoop(int loopStart) {
    emitByte(OP_LOOP); 

    int offset = currentChunk()->count - loopStart + 2;
    if (offset > UINT16_MAX) error("Loop body is too large.");

    emitByte((offset >> 8) & 0xff);
    emitByte(offset & 0xff);
}

static void emitReturn() {
    emitByte(OP_RETURN);
}

static void endCompiler() {
    emitReturn();

#ifdef DEBUG_PRINT_CODE
    if (!parser.hadError) {
        disassembleChunk(currentChunk(), "code");
    }
#endif

}

static void beginScope() {
    current->scopeDepth++;
}

static void endScope() {
    current->scopeDepth--;
    //when we leave a scope we discard all the variables associated with it
    while (current->localCount > 0 && current->locals[current->localCount - 1].depth > current->scopeDepth) {
        emitByte(OP_POP);
        current->localCount--;
    }
}

static void popLocalsAbove (int depth) {
    while (current->localCount > 0 && current->locals[current->localCount - 1].depth > depth) {
        emitByte(OP_POP);
        current->localCount--;
    }
}

static void expression();
static ParseRule* getRule(TokenType type);
static void parsePrecedence(Precedence precedence);


static void grouping(bool canAssign) {
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ') after expression.");
}

static void unary(bool canAssign) {
    TokenType operatorType = parser.previous.type;

    parsePrecedence(PREC_UNARY);

    switch(operatorType) {
        case TOKEN_BANG: emitByte(OP_NOT); break;
        case TOKEN_MINUS: emitByte(OP_NEGATE); break;

        default: return;
    }
}

static void binary(bool canAssign) {

 // op type
    TokenType operatorType = parser.previous.type;

 // compile the right op
    ParseRule* rule = getRule(operatorType);
    parsePrecedence((Precedence)(rule->precedence + 1));


 // emit the op instruction
    switch (operatorType) {
        case TOKEN_BANG_EQUAL:      emitBytes(OP_EQUAL, OP_NOT); break;
        case TOKEN_EQUAL_EQUAL:     emitByte(OP_EQUAL); break;
        case TOKEN_GREATER:         emitByte(OP_GREATER); break;
        case TOKEN_GREATER_EQUAL:   emitBytes(OP_LESS, OP_NOT); break;
        case TOKEN_LESS:            emitByte(OP_LESS); break;
        case TOKEN_LESS_EQUAL:      emitBytes(OP_GREATER, OP_NOT); break;
        case TOKEN_PLUS:            emitByte(OP_ADD); break; 
        case TOKEN_MINUS:           emitByte(OP_SUBTRACT); break;
        case TOKEN_STAR:            emitByte(OP_MULTIPLY); break;
        case TOKEN_SLASH:           emitByte(OP_DIVIDE); break;
        
        default: return;
  }
}

static void ternary(bool canAssign) {

}

static void number(bool canAssign) {
    double value = strtod(parser.previous.start, NULL);
    emitConstant(NUMBER_VAL(value));
}

static void literal(bool canAssign) {
    switch (parser.previous.type) {
        case TOKEN_FALSE:   emitByte(OP_FALSE); break;
        case TOKEN_NIL:     emitByte(OP_NIL); break;
        case TOKEN_TRUE:    emitByte(OP_TRUE); break;

        default: return;
    }
}

static void string(bool canAssign) {
    emitConstant(OBJ_VAL(copyString(parser.previous.start + 1, parser.previous.length - 2)));
}


static void namedVariable(Token name, bool canAssign) {

    int arg = resolveLocal(current, &name); 
    
    if (arg != -1) {
        
        if (canAssign && checkMatch(TOKEN_EQUAL)) {
            if (current->locals[arg].isConst) {
                error("Cannot assign to const variable.");
                return;
            }
            expression();
            emitBytes(OP_SET_LOCAL, (uint8_t)arg);
        } else {
            emitBytes(OP_GET_LOCAL, (uint8_t)arg);
        }
    } else {
    
        uint32_t globalIndex = identifierConstant(&name);
        
        if (globalIndex <= UINT8_MAX) {
            if (canAssign && checkMatch(TOKEN_EQUAL)) {
                expression();
                emitBytes(OP_SET_GLOBAL, (uint8_t)globalIndex);
            } else {
                emitBytes(OP_GET_GLOBAL, (uint8_t)globalIndex);
            }
        } else {
            if (canAssign && checkMatch(TOKEN_EQUAL)) {
                expression();
                emitFourBytes(OP_SET_GLOBAL_LONG,  (uint8_t)((globalIndex & 0x000000ff)), 
                                                   (uint8_t)((globalIndex & 0x0000ff00) >> 8), 
                                                   (uint8_t)((globalIndex & 0x00ff0000) >> 16));

            } else {
                emitFourBytes(OP_GET_GLOBAL_LONG,  (uint8_t)((globalIndex & 0x000000ff)), 
                                                   (uint8_t)((globalIndex & 0x0000ff00) >> 8), 
                                                   (uint8_t)((globalIndex & 0x00ff0000) >> 16));

            }
        }
    }
}

static void variable(bool canAssign) {
    namedVariable(parser.previous, canAssign);
}


static void addLocal(Token name, bool isConst) {
    //we add the local to the current scope
    Local* local = &current->locals[current->localCount++];
    local->name = name;
    //marked as uninitialized with depth = -1. Variable is "ready for use" after its
    //initializer is compiled
    local->depth = -1;
    local->isConst = isConst;
}

static void markInitialized() {
    current->locals[current->localCount - 1].depth = current->scopeDepth;
}

static void declareVariable(bool isConst) {
    //globals are implicitly declared
    if (current->scopeDepth == 0) return;

    Token* name = &parser.previous;
    //if we don't find another variable with the same name in this scope, 
    //we add the variable to the locals array 
    for (int i = current->localCount - 1; i >= 0; i--) {
        Local* local = &current->locals[i];
        if (local->depth != -1 && local->depth < current->scopeDepth) {
            break;
        }

        if (identifiersEqual(name, &local->name)) {
            error("Another variable with this name is in this scope.");
        }
    }

    addLocal(*name, isConst);
}

static uint32_t parseVariable(const char* errorMessage, bool isConst) {
    consume(TOKEN_IDENTIFIER, errorMessage);

    declareVariable(isConst);
    if (current->scopeDepth > 0) return -1;

    return identifierConstant(&parser.previous);
}

static void defineVariable(uint32_t global, bool isConst) {
    if (current->scopeDepth > 0) {
        markInitialized();
        return;
    }

    if (isConst) {
        if (global < UINT8_MAX) {
            emitBytes(OP_DEFINE_CONST_GLOBAL, (uint8_t)global);
        } else {
            emitFourBytes(OP_DEFINE_CONST_GLOBAL_LONG, (uint8_t)((global & 0x000000ff)), 
                                                       (uint8_t)((global & 0x0000ff00) >> 8), 
                                                       (uint8_t)((global & 0x00ff0000) >> 16));
        }
    } else {
        if (global < UINT8_MAX) {
            emitBytes(OP_DEFINE_GLOBAL, (uint8_t)global);
        } else {
            emitFourBytes(OP_DEFINE_GLOBAL_LONG, (uint8_t)((global & 0x000000ff)), 
                                                 (uint8_t)((global & 0x0000ff00) >> 8), 
                                                 (uint8_t)((global & 0x00ff0000) >> 16));
        }
        
    } 
}

static void varDeclaration(bool isConst) {
    uint32_t global = parseVariable("Expect a variable name.", isConst);

    if (checkMatch(TOKEN_EQUAL)) {
        expression();
    } else {
        emitByte(OP_NIL);
    }

    consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

    defineVariable(global, isConst);
}

static void and_(bool canAssign) {
    int endJump = emitJump(OP_JUMP_IF_FALSE);

    emitByte(OP_POP);
    parsePrecedence(PREC_AND);

    patchJump(endJump);
}

static void or_(bool canAssign) {
    int elseJump = emitJump(OP_JUMP_IF_FALSE);
    int endJump = emitJump(OP_JUMP);

    patchJump(elseJump);
    emitByte(OP_POP);
    
    parsePrecedence(PREC_OR);
    patchJump(endJump);

    //se l'operando di sinistra è vero -> endJump : return
    //else -> elseJump : parsePrecedence, return
}

static ParseRule* getRule(TokenType type) {
    return &rules[type];
}

static void parsePrecedence(Precedence precedence) {
    advance();
    ParseFn prefixRule = getRule(parser.previous.type)->prefix;
    if (prefixRule == NULL) {
        error("Expect expression.");
        return;
    }

    //if parsePrecedence is called with precedence > than assignement
    //canAssign is false, skips the infix loop and correctly 
    //communicates the error of invalid assignement
    bool canAssign = precedence <= PREC_ASSIGNMENT;
    prefixRule(canAssign);

    while (precedence <= getRule(parser.current.type)->precedence) {
        advance();
        ParseFn infixRule = getRule(parser.previous.type)->infix;
        infixRule(canAssign);
    }

    if (canAssign && checkMatch(TOKEN_EQUAL)) {
        error("Invalid assignement target.");
    }
}


static void statement();
static void loopStatement(int loopStart, BreakEntries* breakEntries);

static void expressionStatement() {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
    emitByte(OP_POP);
}

static void printStatement() {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after value.");
    emitByte(OP_PRINT);
}

static void ifStatement(bool insideLoop, int loopStart, BreakEntries* breakEntries) {
    
    consume(TOKEN_LEFT_PAREN, "Expect '(' before condition.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    int thenJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);

    if (insideLoop) {
        loopStatement(loopStart, breakEntries);
    } else statement();
    
    //backpatching for the then block and else block
    int elseJump = emitJump(OP_JUMP);

    patchJump(thenJump);

    if (checkMatch(TOKEN_ELSE)) {
        if (insideLoop) {
            loopStatement(loopStart, breakEntries);
        } else {
            statement();
        }
    }

    patchJump(elseJump);
}

static void whileStatement() {
    
    BreakEntries breakEntries = {.breakCount = 0, .depth = 0};
    int loopStart = currentChunk()->count;

    consume(TOKEN_LEFT_PAREN, "Expect '(' before condition.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    int exitJump = emitJump(OP_JUMP_IF_FALSE);

    emitByte(OP_POP);

    breakEntries.depth = current->scopeDepth;
    loopStatement(loopStart, &breakEntries);

    emitLoop(loopStart);

    patchJump(exitJump);
    emitByte(OP_POP);

    for (int i = 0; i < breakEntries.breakCount; i++) {
        patchJump(breakEntries.breakJumps[i]);
    }

    

}

static void forStatement() {
    
    beginScope();
    BreakEntries breakEntries = {.breakCount = 0, .depth = 0};

    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");
    
    // Initializer clause
    if (checkMatch(TOKEN_SEMICOLON)) {
        // Empty initializer - do nothing
    } else if (checkMatch(TOKEN_CONST)) {
        consume(TOKEN_VAR, "Expect 'var' after 'const'.");
        varDeclaration(true);  // This will consume the semicolon
    } else if (checkMatch(TOKEN_VAR)) {
        varDeclaration(false); // This will consume the semicolon
    } else {
        printf("DEBUG: Calling expressionStatement for initializer\n");
        expressionStatement(); // This already consumes the semicolon
    }

    // At this point we should be at the condition
    
    int loopStart = currentChunk()->count;

    // Condition clause
    int exitJump = -1;
    if (!checkMatch(TOKEN_SEMICOLON)) {
        
        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");
        
        exitJump = emitJump(OP_JUMP_IF_FALSE);
        emitByte(OP_POP);
    }

    
    // Increment clause
    if (!checkMatch(TOKEN_RIGHT_PAREN)) {
        
        int bodyJump = emitJump(OP_JUMP);
        
        int incrementStart = currentChunk()->count;
        expression();
        emitByte(OP_POP);
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");
        
        emitLoop(loopStart);
        loopStart = incrementStart;
        patchJump(bodyJump);
    }

    breakEntries.depth = current->scopeDepth;
    loopStatement(loopStart, &breakEntries);
    emitLoop(loopStart);
    

    if (exitJump != -1) {
        patchJump(exitJump);
        emitByte(OP_POP); // pops condition result

    } 

    
    for (int i = 0; i < breakEntries.breakCount; i++) {
        patchJump(breakEntries.breakJumps[i]); // breaks land after the pop
    }

    
    endScope();
}



static void block();
static void loopBlock(int loopStart, BreakEntries* breakEntries);

static void statement() {
    
    if (checkMatch(TOKEN_PRINT)) {
        printStatement();
    } else if(checkMatch(TOKEN_IF)) {
        //we're not inside a loop, so insideLoop false and we don't have a loopStart and break entries
        ifStatement(false, -1, NULL);
    } else if(checkMatch(TOKEN_WHILE)) {
        whileStatement();
    } else if(checkMatch(TOKEN_FOR)) {
        forStatement();
    }  else if (checkMatch(TOKEN_LEFT_BRACE)) {
        beginScope();
        block();
        endScope();
    } else {
        expressionStatement();  
    }
}

static void loopStatement(int loopStart, BreakEntries* breakEntries) {
    if (checkMatch(TOKEN_PRINT)) {
        printStatement();
    } else if(checkMatch(TOKEN_IF)) {
        ifStatement(true, loopStart, breakEntries);
    } else if(checkMatch(TOKEN_WHILE)) {
        whileStatement();
    } else if(checkMatch(TOKEN_FOR)) {
        forStatement();
    } else if(checkMatch(TOKEN_CONTINUE)) {
        consume(TOKEN_SEMICOLON, "Expect ';' after statement.");
        emitLoop(loopStart);
    } else if(checkMatch(TOKEN_BREAK)) {
        consume(TOKEN_SEMICOLON, "Expect ';' after statement.");
        popLocalsAbove(breakEntries->depth);
        breakEntries->breakJumps[breakEntries->breakCount] = emitJump(OP_JUMP);
        breakEntries->breakCount++;
    } else if (checkMatch(TOKEN_LEFT_BRACE)) {
        beginScope();
        loopBlock(loopStart, breakEntries);
        endScope();
    } else {
        expressionStatement();  
    }
}



static void synchronize() {
    parser.panicMode = false;
    while (parser.current.type != TOKEN_EOF) {
        if (parser.previous.type  == TOKEN_SEMICOLON) return;
        
        switch (parser.current.type) {
            case TOKEN_CLASS:
            case TOKEN_FUNC:
            case TOKEN_VAR:
            case TOKEN_CONST:
            case TOKEN_FOR:
            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_PRINT:
            case TOKEN_RETURN:
                return;

            default:
            //nothing. The parser continues to search for a statement
            break;
        }

        advance();
    }
}

static void declaration () {

    if (checkMatch(TOKEN_CONST)) {
        consume(TOKEN_VAR, "Expect variable after const.");
        varDeclaration(true);
    } else if (checkMatch(TOKEN_VAR)) {
        varDeclaration(false);
    } else {
        statement();
    }

    //the synchronization point for the parser is the end of a statement
    //synchronization is needed because a syntax error could lead
    //to potential misinterpretations and cascade errors.
    if (parser.panicMode) synchronize();
}

static void loopDeclaration(int loopStart, BreakEntries* breakEntries) {

    if (checkMatch(TOKEN_CONST)) {
        consume(TOKEN_VAR, "Expect variable after const.");
        varDeclaration(true);
    } else if (checkMatch(TOKEN_VAR)) {
        varDeclaration(false);
    } else {
        loopStatement(loopStart, breakEntries);
    }

    //the synchronization point for the parser is the end of a statement
    //synchronization is needed because a syntax error could lead
    //to potential misinterpretations and cascade errors.
    if (parser.panicMode) synchronize();
}

static void block() {
    while(!checkType(TOKEN_RIGHT_BRACE) && !checkType(TOKEN_EOF)) {
        declaration();
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void loopBlock(int loopStart, BreakEntries* breakEntries) {
    while(!checkType(TOKEN_RIGHT_BRACE) && !checkType(TOKEN_EOF)) {
        loopDeclaration(loopStart, breakEntries);
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void expression() {
    parsePrecedence(PREC_ASSIGNMENT);
}



static void initCompiler(Compiler* compiler) {
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    current = compiler;
}

bool compile(const char* source, Chunk* chunk) {
    initScanner(source);
    Compiler compiler;
    initCompiler(&compiler);
    compilingChunk = chunk;

    parser.hadError = false;
    parser.panicMode = false;

    advance();

    while (!checkMatch(TOKEN_EOF)) {
        declaration();
    }

    endCompiler();
    return !parser.hadError;

}