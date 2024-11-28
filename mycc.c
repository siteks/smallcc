
#include "mycc.h"


void error(char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}


Token   *token;
Node    *code[100];
char    *user_input;
Local   *locals;


int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: num\n");
        return 1;
    }
    
    user_input = argv[1];
    token = tokenise(user_input);
    locals = NULL;
    print_tokens();

    //Node *node = expr();
    program();
    print_tree(code[0], 0);
    print_locals();

    printf(".text=0\n");
    printf("    li      r6 0x0\n");
    printf("    lih     r6 0x10\n");
    printf("    bl      r5 main\n");
    printf("    halt\n");
    printf(".text=0x20\n");
    printf("main:\n");

    int i = 0;
    gen_preamble(locals ? locals->offset + 1 : 0);
    while(code[i])
    {
        gen_code(code[i++]);
    }
    gen_postamble();
    gen_pop(0);
    printf("    halt\n");

    return 0;
}


