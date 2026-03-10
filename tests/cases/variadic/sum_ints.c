// EXPECT_R0: 15
int vadd3(int n,...){va_list ap;int s;int i;s=0;va_start(ap,n);for(i=0;i<n;i++)s+=va_arg(ap,int);va_end(ap);return s;} int main(){return vadd3(3,4,5,6);}
