// EXPECT_R0: 60
int sum(int n,...){va_list ap;int s;int i;s=0;va_start(ap,n);for(i=0;i<n;i++)s+=va_arg(ap,int);va_end(ap);return s;} int main(){return sum(3,10,20,30);}
