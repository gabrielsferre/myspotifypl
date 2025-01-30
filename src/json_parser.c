typedef enum json_ElementType {
    json_INVALID_ELEMENT,
    json_ARRAY,
    json_OBJECT,
    json_NUMBER,
    json_TRUE,
    json_FALSE,
    json_NULL,
    json_STRING,
} json_ElementType;

typedef struct json_Element {
    json_ElementType type;
    Buffer label;
    Buffer value;
    struct json_Element *firstSubElement;
    struct json_Element *nextSibling;
} json_Element;

typedef struct Cursor {
    Buffer buf;
    u64 offset;
    b32 errorOccurred;
} Cursor;

typedef enum TokenType {
    TK_INVALID,
    TK_END,

    TK_OPEN_BRACE,
    TK_CLOSE_BRACE,
    TK_OPEN_BRACKET,
    TK_CLOSE_BRACKET,
    TK_STRING,
    TK_COLON,
    TK_COMMA,
    TK_TRUE,
    TK_FALSE,
    TK_NULL,
    TK_KEYWORD,
    TK_NUMBER,
} TokenType;

typedef struct Token {
    TokenType type;
    Buffer content;
} Token;

static void
parsingError(Cursor *cur, char const *message)
{
    if(!cur->errorOccurred) {
        fprintf(stderr, "JSON ERROR: %s\n", message);
    }
    cur->errorOccurred = 1;
}

static char
peek(Cursor const *cur)
{
    return (char)cur->buf.data[cur->offset];
}

static b32
isCursorEnd(Cursor const *cur)
{
    return !isInBounds(cur->buf, cur->offset) || peek(cur) == '\0';
}

static b32
isSpace(char ch)
{
    return
        ch == ' '  ||
        ch == '\t' ||
        ch == '\n' ||
        ch == '\r' ||
        ch == '\v';
}

static b32
isSeparator(char ch)
{
    return
        isSpace(ch) ||
        ch == ',' ||
        ch == '[' ||
        ch == ']' ||
        ch == '{' ||
        ch == '}' ||
        ch == ':';
}

static b32
isNumeric(char ch)
{
    return ch >= '0' && ch <= '9';
}

static void
skipSpaces(Cursor *cur)
{
    if(isCursorEnd(cur)) {
        return;
    }
    char ch = peek(cur);
    while((isSpace(ch) || !ch) && !isCursorEnd(cur)) {
        cur->offset += 1;
        ch = peek(cur);
    }
}

static TokenType
parseString(Cursor *cur)
{
    // NOTE: this function lets strings occupy multiple lines, which isn't right
    char ch = peek(cur);
    check(ch == '"');
    cur->offset += 1;
    ch = peek(cur);
    // NOTE: possible off-by-one kind of erro here, that's why we should allocate a bigger buffer than the actual file size
    while(ch != '"' && !isCursorEnd(cur)) {
        if(ch == '\\') {
            cur->offset += 2;
        }
        else {
            cur->offset += 1;
        }
        ch = peek(cur);
    }
    if(ch == '"') {
        cur->offset += 1;
        return TK_STRING;
    }
    else {
        return TK_INVALID;
    }
}

static TokenType
parseKeyword(Cursor *cur, Buffer buf)
{
    Buffer buf2 = {0};
    buf2.data = cur->buf.data + cur->offset;
    char ch = peek(cur);
    while(!isSeparator(ch) && !isCursorEnd(cur)) {
        buf2.count += 1;
        cur->offset += 1;
        ch = peek(cur);
    }
    if(areEqual(buf, buf2)) {
        return TK_KEYWORD;
    }
    else {
        return TK_INVALID;
    }
}

static TokenType
parseNumber(Cursor *cur)
{
    char ch = peek(cur);
    if(ch == '-') {
        cur->offset += 1;
        ch = peek(cur);
    }
    if(!isNumeric(ch)) {
        return TK_INVALID;
    }
    while(isNumeric(ch) && !isCursorEnd(cur)) {
        cur->offset += 1;
        ch = peek(cur);
    }
    if(ch != '.' && ch != 'e' && ch != 'E') {
        return TK_NUMBER;
    }
    if(ch == '.') {
        cur->offset += 1;
        ch = peek(cur);
        if(!isNumeric(ch)) {
            return TK_INVALID;
        }
        while(isNumeric(ch) && !isCursorEnd(cur)) {
            cur->offset += 1;
            ch = peek(cur);
        }
    }
    if(ch == 'e' || ch == 'E') {
        cur->offset += 1;
        ch = peek(cur);
        if(ch == '+' || ch == '-') {
            cur->offset += 1;
            ch = peek(cur);
        }
        if(!isNumeric(ch)) {
            return TK_INVALID;
        }
        while(isNumeric(ch) && !isCursorEnd(cur)) {
            cur->offset += 1;
            ch = peek(cur);
        }
    }
    if(!isSeparator(ch) && !isCursorEnd(cur)) {
        return TK_INVALID;
    }
    return TK_NUMBER;
}

static Token
parseNextToken(Cursor *cur) {
    Token tk = {0};
    skipSpaces(cur);
    if(isCursorEnd(cur)) {
        tk.type = TK_END;
        return tk;
    }
    tk.content.data = cur->buf.data + cur->offset;
    u64 initialOffset = cur->offset;
    char ch = peek(cur);
    switch(ch) {
        case '{':
        {
            tk.type = TK_OPEN_BRACE;
            cur->offset += 1;
            break;
        }
        case '}':
        {
            tk.type = TK_CLOSE_BRACE;
            cur->offset += 1;
            break;
        }
        case '[':
        {
            tk.type = TK_OPEN_BRACKET;
            cur->offset += 1;
            break;
        }
        case ']':
        {
            tk.type = TK_CLOSE_BRACKET;
            cur->offset += 1;
            break;
        }
        case ':':
        {
            tk.type = TK_COLON;
            cur->offset += 1;
            break;
        }
        case ',':
        {
            tk.type = TK_COMMA;
            cur->offset += 1;
            break;
        }
        case '"':
        {
            tk.type = parseString(cur);
            break;
        }
        case 't':
        {
            tk.type = parseKeyword(cur, CONSTANT_STRING("true"));
            if(tk.type == TK_KEYWORD) {
                tk.type = TK_TRUE;
            }
            break;
        }
        case 'f':
        {
            tk.type = parseKeyword(cur, CONSTANT_STRING("false"));
            if(tk.type == TK_KEYWORD) {
                tk.type = TK_FALSE;
            }
            break;
        }
        case 'n':
        {
            tk.type = parseKeyword(cur, CONSTANT_STRING("null"));
            if(tk.type == TK_KEYWORD) {
                tk.type = TK_NULL;
            }
            break;
        }
        case '-':
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
        {
            tk.type = TK_NUMBER;
            parseNumber(cur);
            break;
        }
    }

    b32 didntMatch = cur->offset == initialOffset;
    if(didntMatch) {
        tk.type = TK_INVALID;
        cur->offset += 1;
    }

    tk.content.count = cur->offset - initialOffset;
    return tk;
}

static Buffer
takeOffQuotes(Buffer buf) {
    buf.data += 1;
    buf.count -= 2;
    return buf;
}

static json_Element parseList(
        MemoryArena *arena, Cursor *cur, json_ElementType listType);

static json_Element
parseElementValue(MemoryArena *arena, Cursor *cur, Token elementValueTk)
{
    json_Element element = {0};
    switch(elementValueTk.type) {
        case TK_NUMBER:
        {
            element.type = json_NUMBER;
            Buffer copy = pushBuffer(arena, elementValueTk.content);
            element.value = copy;
            break;
        }
        case TK_STRING:
        {
            element.type = json_STRING;
            Buffer copy =
                pushBuffer(arena, takeOffQuotes(elementValueTk.content));
            element.value = copy;
            break;
        }
        case TK_FALSE:
        {
            element.type = json_FALSE;
            break;
        }
        case TK_TRUE:
        {
            element.type = json_TRUE;
            break;
        }
        case TK_NULL:
        {
            element.type = json_NULL;
            break;
        }
        case TK_OPEN_BRACE:
        {
            element = parseList(arena, cur, json_OBJECT);
            break;
        }
        case TK_OPEN_BRACKET:
        {
            element = parseList(arena, cur, json_ARRAY);
            break;
        }
        default:
        {
            parsingError(cur, "invalid element value");
            break;
        }
    }
    return element;
}

static json_Element
parseList(MemoryArena *arena, Cursor *cur, json_ElementType listType)
{
    check(listType == json_ARRAY || listType == json_OBJECT);

    json_Element listElement = {0};
    listElement.type = listType;

    TokenType closingTkType =
        (listType == json_OBJECT) ? TK_CLOSE_BRACE : TK_CLOSE_BRACKET;

    Token tk = parseNextToken(cur);
    if(tk.type == closingTkType) {
        return listElement;
    }

    Token separatorTk = {0};
    json_Element *lastSubElement = 0;
    while(!isCursorEnd(cur)) {
        Buffer label = {0};
        if(listType == json_OBJECT) {
            Token labelTk = tk;
            Token colonTk = parseNextToken(cur);
            if(labelTk.type != TK_STRING) {
                parsingError(cur, "expected string as label");
            }
            else if(colonTk.type != TK_COLON) {
                parsingError(cur, "expected colon after label");
            }
            label = takeOffQuotes(labelTk.content);

            tk = parseNextToken(cur);
        }
        Token elementValueTk = tk;
        json_Element *subElement = pushStruct(arena, json_Element);
        check(subElement);
        *subElement = parseElementValue(arena, cur, elementValueTk);
        subElement->label = pushBuffer(arena, label);
        if(lastSubElement) {
            lastSubElement->nextSibling = subElement;
        }
        else {
            listElement.firstSubElement = subElement;
        }
        lastSubElement = subElement;
        separatorTk = parseNextToken(cur);
        if(separatorTk.type != TK_COMMA && separatorTk.type != closingTkType) {
            parsingError(cur, "expected a comma");
        }
        if(separatorTk.type == closingTkType) {
            break;
        }
        tk = parseNextToken(cur);
    }
    if(separatorTk.type != closingTkType) {
        parsingError(cur, "list was not closed");
    }
    return listElement;
}

json_Element*
json_parseJson(MemoryArena *arena, Buffer jsonString) {
    Cursor cur = {0};
    cur.buf = jsonString;
    Token tk = parseNextToken(&cur);
    if(tk.type != TK_OPEN_BRACE && tk.type != TK_OPEN_BRACKET)
    {
        parsingError(&cur, "expected opening list");
        return 0;
    }
    json_Element *head = pushStruct(arena, json_Element);
    check(head);
    *head = parseElementValue(arena, &cur, tk);
    return head;
}

static f64
takePower(f64 base, s64 exponent)
{
    if(base == 0) {
        return 0;
    }

    s64 exponentSign = (exponent < 0) ? -1 : 1;
    if(exponent < 0) {
        exponent = -exponent;
    }
    f64 a = 1;
    // the product a*(base^exponent) is invariant on the loop
    while(exponent > 0) {
        if(exponent % 2 == 0) {
            base = base * base;
            exponent = exponent / 2;
        }
        else {
            a = (exponentSign == 1) ? a * base : a / base;
            exponent -= 1;
        }
    }
    return a;
}

f64
json_getNumber(json_Element element)
{
    if(element.type != json_NUMBER) {
        return 0;
    }
    Buffer buf = element.value;
    u64 offset = 0;
    char *data = (char*)buf.data;
    char lastRead = data[offset];
    s64 resultSign = 1;
    if(lastRead == '-') {
        resultSign = -1;
        offset += 1;
    }
    f64 integerPart = 0;
    while(isInBounds(buf, offset)) {
        lastRead = data[offset++];
        if(!isNumeric(lastRead)) {
            break;
        }
        integerPart = (lastRead - '0') + 10*integerPart;
    }
    f64 decimalPart = 0;
    if(lastRead == '.') {
        f64 c = 1;
        while(isInBounds(buf, offset)) {
            lastRead = data[offset++];
            if(!isNumeric(lastRead)) {
                break;
            }
            c *= 0.1;
            decimalPart += c*(lastRead - '0');
        }
    }

    s64 exponent = 0;
    s64 exponentSign = 1; 
    if(lastRead == 'e' || lastRead == 'E') {
        lastRead = data[offset];
        if(lastRead == '+' || lastRead == '-') {
            offset += 1;
            if(lastRead == '-') {
                exponentSign = -1;
            }
        }
        while(isInBounds(buf, offset)) {
            lastRead = data[offset++];
            if(!isNumeric(lastRead)) {
                break;
            }
            exponent = (lastRead - '0') + 10*exponent;
        }
    }
    f64 result = integerPart + decimalPart;
    result *= takePower(10,exponent*exponentSign);
    result *= resultSign;
    return result;
}

u64
json_getArrayCount(json_Element array)
{
    u64 count = 0;
    if(array.type == json_ARRAY) {
        for(json_Element *element = array.firstSubElement;
                element;
                element = element->nextSibling) {
            count += 1;
        }
    }
    return count;
}

json_Element
json_getElement(json_Element element, Buffer label)
{
    if(element.type == json_OBJECT && label.data) {
        json_Element *newElement = element.firstSubElement;
        while(newElement) {
            if(areEqual(label, newElement->label)) {
                return *newElement;
            }
            newElement = newElement->nextSibling;
        }
    }
    return (json_Element){0};
}

void
json_printElement(json_Element *element)
{
    // NOTE: exchanging this loop by a recursion may result in stack overflow
    while(element) {
        printBuffer(element->label);
        printf(":");
        switch(element->type) {
            case json_INVALID_ELEMENT: break;
            case json_TRUE: printf("true"); break;
            case json_FALSE: printf("false");break;
            case json_NULL: printf("null"); break;
            case json_NUMBER: printf("%f", json_getNumber(*element)); break;
            case json_STRING: printBuffer(element->value); break;
            case json_ARRAY: printf("["); break;
            case json_OBJECT: printf("{"); break;
        }

        printf("\n");
        json_printElement(element->firstSubElement);

        switch(element->type) {
            case json_ARRAY: printf("]\n"); break;
            case json_OBJECT: printf("}\n"); break;
            default: break;
        }
        element = element->nextSibling;
    }
}
