
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

    Node *node = program();
    print_tree(node, 0);


    printf(".text=0\n");
    printf("    li      r6 0x0\n");
    printf("    lih     r6 0x10\n");
    printf("    bl      r5 main\n");
    printf("    halt\n");
    printf(".text=0x20\n");

    // int i = 0;
    // gen_preamble(locals ? locals->offset + 1 : 0);
    gen_code(node);
    // gen_postamble();
    // gen_pop(0);
    // printf("    halt\n");

    return 0;
}


