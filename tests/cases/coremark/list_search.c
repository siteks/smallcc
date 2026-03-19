// EXPECT_R0: 2
// Linked list: array-allocated nodes, search by value, return matching id
struct Node { int val; int id; struct Node *next; };
struct Node *find(struct Node *head, int target) {
    struct Node *p=head;
    while (p!=0) { if (p->val==target) return p; p=p->next; }
    return 0;
}
int main() {
    struct Node nodes[5];
    struct Node *found;
    int i;
    for (i=0;i<5;i=i+1) {
        nodes[i].val=(i+2)*(i+1);   /* 2,6,12,20,30 */
        nodes[i].id=i;
        nodes[i].next=(i<4)?&nodes[i+1]:0;
    }
    found=find(&nodes[0],12);
    if (found==0) return -1;
    return found->id;
}
