// EXPECT_R0: 80
// 4x4 matrix multiply: a[i][j]=i+j+1, b=diag(2)+ones; return trace of c
#define N 4
int a[N][N], b[N][N], c[N][N];
void mat_mul(void) {
    int i,j,k;
    for (i=0;i<N;i=i+1) for (j=0;j<N;j=j+1) {
        c[i][j]=0;
        for (k=0;k<N;k=k+1) c[i][j]=c[i][j]+a[i][k]*b[k][j];
    }
}
int main() {
    int i,j,trace;
    for (i=0;i<N;i=i+1) for (j=0;j<N;j=j+1) {
        a[i][j]=i+j+1;
        b[i][j]=(i==j)?2:1;
    }
    mat_mul();
    trace=0;
    for (i=0;i<N;i=i+1) trace=trace+c[i][i];
    return trace;
}
