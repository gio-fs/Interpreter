#ifndef clox_scanner_h
#define clox_scanner_h

typedef enum {

    // Single-character tokens.
    TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN,
    TOKEN_LEFT_BRACE, TOKEN_RIGHT_BRACE,
    TOKEN_LEFT_SQUARE_BRACE, TOKEN_RIGHT_SQUARE_BRACE,
    TOKEN_COMMA, TOKEN_DOT, TOKEN_MINUS, TOKEN_PLUS,
    TOKEN_SEMICOLON, TOKEN_SLASH, TOKEN_STAR, TOKEN_COLON,
    // One or two character tokens.
    TOKEN_BANG, TOKEN_BANG_EQUAL,
    TOKEN_EQUAL, TOKEN_EQUAL_EQUAL,
    TOKEN_GREATER, TOKEN_GREATER_EQUAL,
    TOKEN_LESS, TOKEN_LESS_EQUAL,
    TOKEN_TERNARY, 
    // Literals.
    TOKEN_IDENTIFIER, TOKEN_STRING, TOKEN_NUMBER, TOKEN_STRING_WITH_INTERP, TOKEN_STRING_INTERP_START,
    // Keywords.
    TOKEN_AND, TOKEN_CLASS, TOKEN_ELSE, TOKEN_FALSE,
    TOKEN_FOR, TOKEN_FUNC, TOKEN_IF, TOKEN_NIL, TOKEN_OR,
    TOKEN_PRINT, TOKEN_RETURN, TOKEN_SUPER, TOKEN_THIS,
    TOKEN_TRUE, TOKEN_VAR, TOKEN_WHILE, TOKEN_CONST,
    TOKEN_CONTINUE, TOKEN_SWITCH, TOKEN_BREAK, TOKEN_LAMBDA,

    TOKEN_ERROR,
    NULL_TOKEN,
    TOKEN_EOF

} TokenType;

typedef struct {
    TokenType type;
    const char* start;
    int length;
    int line;
} Token;

typedef struct {
    const char* start;
    const char* current;
    int line;
    bool isInInterpolation;
    bool scannedInterpEnd;
} Scanner;




void initScanner(const char* source);
Token scanToken();

#endif
