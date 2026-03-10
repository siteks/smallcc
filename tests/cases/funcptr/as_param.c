// EXPECT_R0: 9
int sq(int x){return x*x;} int apply(int(*f)(int),int v){return f(v);} int main(){return apply(sq,3);}
