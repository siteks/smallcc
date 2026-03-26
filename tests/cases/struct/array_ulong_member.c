// EXPECT_R0: 1
/* Array of structs with unsigned long member assignment and check */
typedef struct S { int a1; int a2; int s3; void * m[4]; unsigned long b; } cr;

static ee_u16 list_known_crc[]   = { (ee_u16)0xd4b0,
                                   (ee_u16)0x3340,
                                   (ee_u16)0x6a79,
                                   (ee_u16)0xe714,
                                   (ee_u16)0xe3c1 };
static ee_u16 matrix_known_crc[] = { (ee_u16)0xbe52,
                                     (ee_u16)0x1199,
                                     (ee_u16)0x5608,
                                     (ee_u16)0x1fd7,
                                     (ee_u16)0x0747 };
static ee_u16 state_known_crc[]  = { (ee_u16)0x5e47,
                                    (ee_u16)0x39bf,
                                    (ee_u16)0xe5a4,
                                    (ee_u16)0x8e3a,
                                    (ee_u16)0x8d84 };
                                    
char *mem_name[3] = { "Static", "Heap", "Stack" };

int main() {
    cr arr[1];
    char block[10000];
    arr[0].a1 = 10;
    arr[0].b = 0x12345678;

    /* Check that arr[1].b was set correctly */
    if (arr[0].b == 0x12345678) {
        return 1;  /* Success */
    }
    return 0;  /* Failure */
}
