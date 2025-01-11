
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


extern Token           *token;
extern char            *user_input;
extern Symbol_table    *symbol_table;
extern Symbol_table    *curr_scope_st;

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: num\n");
        return 1;
    }
    
    user_input = argv[1];
    token = tokenise(user_input);
    print_tokens();
    symbol_table = calloc(1, sizeof(Symbol_table));
    curr_scope_st = symbol_table;
    
    make_basic_types();
    Node *node = program();
    print_tree(node, 0);
    print_symbol_table(symbol_table, 0);
    print_type_table();
    propagate_types(0, node);
    // get_types_and_symbols(node);
    print_tree(node, 0);
    print_symbol_table(symbol_table, 0);
    print_type_table();


    printf(".text=0\n");
    printf("    ssp     0x1000\n");
    printf("    jl      main\n");
    printf("    halt\n");

    gen_code(node);

    return 0;
}


