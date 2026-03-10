// EXPECT_R0: 9
int add2(int a,...){va_list ap;int b;va_start(ap,a);b=va_arg(ap,int);va_end(ap);return a+b;} int main(){return add2(4,5);}
