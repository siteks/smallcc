// EXPECT_R0: 30
int vmax(int n,...){va_list ap;int m;int i;int v;va_start(ap,n);m=va_arg(ap,int);for(i=1;i<n;i++){v=va_arg(ap,int);if(v>m)m=v;}va_end(ap);return m;} int main(){return vmax(3,10,30,20);}
