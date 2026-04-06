// EXPECT_R0: 40
// braun's flush_for_call_n spills the live vstack value (5) to an adjw-region slot before the call;
// insert_callee_saves later expands the frame and shifts that slot's bp offset.
// If offset shifting is wrong, the reloaded value is corrupted.
int sum3(int a, int b, int c) { return a + b + c; }
int main(void) {
    return 5 + sum3(10, 15, 10);
}
