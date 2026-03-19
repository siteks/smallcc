// EXPECT_R0: 15
// Linked list: build 5-node list on stack, traverse with pointer, sum vals
struct Node { int val; struct Node *next; };
int main() {
    struct Node a, b, c, d, e;
    struct Node *p;
    int sum;
    a.val=1; a.next=&b;
    b.val=2; b.next=&c;
    c.val=3; c.next=&d;
    d.val=4; d.next=&e;
    e.val=5; e.next=0;
    p=&a; sum=0;
    while (p!=0) { sum=sum+p->val; p=p->next; }
    return sum;
}
