// EXPECT_R0: 9
struct I{int x;int y;}; struct O{struct I p;int z;} g={{3,3},3}; int main() { return g.p.x+g.p.y+g.z; }
