// EXPECT_R0: 8
int double2(int x){return x+x;} int main(){int(*fp)(int);fp=double2;return fp(4);}
