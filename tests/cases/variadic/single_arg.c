// EXPECT_R0: 7
int first(int n,...){va_list ap;int v;va_start(ap,n);v=va_arg(ap,int);va_end(ap);return v;} int main(){return first(1,7);}
