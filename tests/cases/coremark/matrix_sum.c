// EXPECT_R0: 14

typedef int ee_s16;
typedef unsigned long ee_u32;
typedef long ee_s32;
typedef ee_s16 MATDAT;
typedef ee_s32 MATRES;

ee_s16 matrix_sum(ee_u32 N, MATRES *C, MATDAT clipval)
{
    MATRES tmp = 0, prev = 0, cur = 0;
    ee_s16 ret = 0;
    ee_u32 i, j;
    for (i = 0; i < N; i++)
    {
        for (j = 0; j < N; j++)
        {
            cur = C[i * N + j];
            tmp += cur;
            if (tmp > clipval)
            {
                ret += 10;
                tmp = 0;
            }
            else
            {
                ret += (cur > prev) ? 1 : 0;
            }
            prev = cur;
        }
    }
    return ret;
}

#define SIZE 4
int main()
{
    MATRES res[SIZE*SIZE];
    for(int i = 0; i < SIZE * SIZE; i++)
        res[i] = i % 10;
    MATDAT clipval = 100;
    int a = matrix_sum(SIZE, res, clipval);
    return a;
}

