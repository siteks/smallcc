// EXPECT_R0: 5
int id(int x){return x;} int main(){int(*fp)(int);fp=id;return (*fp)(5);}
