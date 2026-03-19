// EXPECT_R0: 69
// 2x2 matrix multiply: exercises 2D array indexing and triple-nested loop
int main() {
    int a[2][2];
    int b[2][2];
    int c[2][2];
    int i, j, k;
    a[0][0]=1; a[0][1]=2; a[1][0]=3; a[1][1]=4;
    b[0][0]=5; b[0][1]=6; b[1][0]=7; b[1][1]=8;
    for (i=0; i<2; i=i+1)
        for (j=0; j<2; j=j+1) {
            c[i][j]=0;
            for (k=0; k<2; k=k+1)
                c[i][j]=c[i][j]+a[i][k]*b[k][j];
        }
    /* c[0][0]=19, c[1][1]=50 -> 69 */
    return c[0][0]+c[1][1];
}
