#include <stdio.h>
#include <string.h>
#include "common.h"
#include "clox_scanner.h"
#include "table.h"
Scanner scanner;


void initScanner(const char* source) {
    scanner.start = source;
    scanner.current = source;
    scanner.isInInterpolation = false;
    scanner.scannedInterpEnd = false;
    scanner.line = 1;
}

static bool isAtEnd() {
    return *scanner.current == '\0';
}

static Token makeToken(TokenType type) {
    Token token;
    token.type = type;
    token.start = scanner.start;
    token.length = (int)(scanner.current - scanner.start);
    token.line = scanner.line;

    return token;
}

static Token errorToken(const char* message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = (int)strlen(message);
    token.line = scanner.line;

    return token;
}

static char advance() {
    scanner.current++;
    return scanner.current[-1];
}

static bool checkMatch(char expected) {
    if(isAtEnd()) return false;
    if(*scanner.current != expected) return false;

    scanner.current++;
    return true;
}

static char* peek() {
    return (char*)scanner.current;
}

static void skipWhitespace() {

    for (;;) {

        char c = *peek();

        switch (c) {
            case ' ':
            case '\r':
            case '\t':
                advance();
                break;
            case '\n':
                scanner.line++;
                advance();
                break;
            case '/':
                if(*(peek() + 1) == '/') {

                    while (*peek() != '\n' && !isAtEnd()) advance();

                } else if (*(peek() + 1) == '*') {

                    while (*peek() != '*' && *(peek() + 1) != '/' && !isAtEnd()) advance();

                } else {
                    return;
                }
                break;

            default: return;
        }
    }
}


static bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

static bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c == '_');
}


static Token string() {


    while (*peek() != '"' && !isAtEnd()) {

        if (*peek() == '$' && *(peek() + 1) == '{') {
            scanner.isInInterpolation = true;
            return makeToken(TOKEN_STRING_WITH_INTERP);
        }

        if (*peek() == '\n') {
            scanner.line++;
        }

        advance();
    }


    if (isAtEnd()) return errorToken("Unterminated string.");

    advance();
    return makeToken(TOKEN_STRING);
}

static Token number() {

    while(isDigit(*peek())) advance();

    if(*peek() == '.' && isDigit(*(peek() + 1))) {

        advance();
        while(isDigit(*peek())) advance();
    }

    return makeToken(TOKEN_NUMBER);
}

static TokenType checkKeyword(int start, int charLeft, const char* keyChar, TokenType type) {

    if(scanner.current - scanner.start == start + charLeft && memcmp(scanner.start + start, keyChar, charLeft) == 0) {
        return type;
    }

    return TOKEN_IDENTIFIER;
}


Token scanToken();

static TokenType identifierType() {
    switch (scanner.start[0]) {
        case 'a': return checkKeyword(1, 2, "nd", TOKEN_AND);
        case 'b': return checkKeyword(1, 4, "reak", TOKEN_BREAK);
        case 'c':
            if (scanner.current - scanner.start > 1) {
                switch (scanner.start[1]) {
                    case 'l': return checkKeyword(2, 3, "ass", TOKEN_CLASS);
                    case 'o':
                        if (scanner.current - scanner.start > 2) {
                            switch (scanner.start[2]) {
                                case 'n':
                                    if (scanner.current - scanner.start > 3) {
                                        switch(scanner.start[3]) {
                                            case 's': return checkKeyword(3, 2, "st", TOKEN_CONST);
                                            case 't': return checkKeyword(3, 5, "tinue", TOKEN_CONTINUE);
                                        }
                                    }

                            }
                    }
                }
            }
            break;

        case 'e': return checkKeyword(1, 3, "lse", TOKEN_ELSE);
        case 'i': if (scanner.current - scanner.start > 1) {
                switch (scanner.start[1]) {
                    case 'f': return checkKeyword(1, 1, "f", TOKEN_IF);
                    case 'n': return checkKeyword(1, 1, "n", TOKEN_IN);
                }
            }
            break;
        case 'f' :
            if (scanner.current - scanner.start > 1) {
                switch (scanner.start[1]) {
                    case 'a': return checkKeyword(2, 3, "lse", TOKEN_FALSE);
                    case 'o': return checkKeyword(2, 1, "r", TOKEN_FOR);
                    case 'n': return checkKeyword(1, 1, "n", TOKEN_FN);
                }
            }
            break;
        case 'l': return checkKeyword(1, 5, "ambda", TOKEN_LAMBDA);
        case 'm': return checkKeyword(1, 4, "atch", TOKEN_MATCH);
        case 'n': return checkKeyword(1, 2, "il", TOKEN_NIL);
        case 'o': return checkKeyword(1, 1, "r", TOKEN_OR);
        case 'p': return checkKeyword(1, 4, "rint", TOKEN_PRINT);
        case 'r': return checkKeyword(1, 5, "eturn", TOKEN_RETURN);
        case 's': return checkKeyword(1, 4, "uper", TOKEN_SUPER);
        case 't':
            if (scanner.current - scanner.start > 1) {
                switch (scanner.start[1]) {
                    case 'h': return checkKeyword(2, 2, "is", TOKEN_THIS);
                    case 'r': return checkKeyword(2, 2, "ue", TOKEN_TRUE);
                }
            }
            break;

        case 'v': return checkKeyword(1, 2, "ar", TOKEN_VAR);
        case 'w': return checkKeyword(1, 4, "hile", TOKEN_WHILE);
    }

    return TOKEN_IDENTIFIER;
}

static Token identifier() {

    while (isAlpha(*peek()) || isDigit(*peek())) advance();

    return makeToken(identifierType());
}


Token scanToken() {
    skipWhitespace();

    scanner.start = scanner.current;

    if (isAtEnd()) return makeToken(TOKEN_EOF);

    char c = advance();

    if (scanner.scannedInterpEnd) {
        scanner.scannedInterpEnd = false;

        if (c == '"') {
            c = advance();
        } else {
            //scanner.current is now 2 characters after '}', the end of the
            //string interpolation (when '}' is scanned, current points to the char after it, so when the next char is scanned
            //it will be 2 after). We want it scanner.start point to the character after the interp end, thus scanner.current--.
            scanner.current--;
            scanner.start = scanner.current;
            return string();
        }
    }

    if (isDigit(c)) return number();
    if (isAlpha(c)) return identifier();



    switch (c) {
        case '[': return makeToken(TOKEN_LEFT_SQUARE_BRACE);
        case ']': return makeToken(TOKEN_RIGHT_SQUARE_BRACE);
        case '(': return makeToken(TOKEN_LEFT_PAREN);
        case ')': return makeToken(TOKEN_RIGHT_PAREN);
        case '{': return makeToken(TOKEN_LEFT_BRACE);
         case '}': {
            if (scanner.isInInterpolation) {
                scanner.isInInterpolation = false;
                scanner.scannedInterpEnd = true;
                return makeToken(TOKEN_SEMICOLON);

            } else return makeToken(TOKEN_RIGHT_BRACE);
        }
        case ';': return makeToken(TOKEN_SEMICOLON);
        case ',': return makeToken(TOKEN_COMMA);
        case '.': return makeToken(checkMatch('.') ? TOKEN_DOUBLE_DOTS: TOKEN_DOT);
        case '-': return makeToken(checkMatch('=') ? TOKEN_MINUS_EQUAL : TOKEN_MINUS);
        case '+': return makeToken(checkMatch('=') ? TOKEN_PLUS_EQUAL : TOKEN_PLUS);
        case '/': return makeToken(TOKEN_SLASH);
        case '*': return makeToken(TOKEN_STAR);
        case '!': return makeToken(checkMatch('=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
        case '=': switch (c = advance()) {
                    case '>': return makeToken(TOKEN_MATCHES_TO);
                    case '=': return makeToken(TOKEN_EQUAL_EQUAL);
                    default: return makeToken(TOKEN_EQUAL);
                }
        case '<': return makeToken(checkMatch('=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
        case '>': return makeToken(checkMatch('=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
        case '$': return makeToken(checkMatch('{') ? TOKEN_STRING_INTERP_START : TOKEN_ERROR);
        case '?': return makeToken(TOKEN_QUESTION);
        case ':': return makeToken(TOKEN_COLON);
        case '"': return string();
        

    }
}
