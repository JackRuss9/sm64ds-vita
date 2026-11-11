typedef struct Node Node;

struct Node {
    char _pad0[0x8];
    int field8;
    unsigned int fieldC;
    char _pad10[0x2C - 0x10];
    int field2C;
    int field30;
};

extern int FSi_SendCommand(Node *node, int a, int b, int c);

#if defined(SM64DS_PLATFORM_PC)
/* Host: the FSFileID's second word is the third argument. The DS spelling
   reads it at &b + 4, which needs b and the first variadic word side by side
   in the frame -- mwccarm's layout, not GCC's. */
int func_0205d568(Node *node, int b, int c_arg)
#else
int func_0205d568(Node *node, int b, ...)
#endif
{
    unsigned int *p;
    if (b == 0) {
        return 0;
    }
    node->field8 = b;
    {
        int b2 = b;
#if defined(SM64DS_PLATFORM_PC)
        int c = c_arg;
#else
        int c = *(int *)((char *)&b + 4);
#endif
        node->field2C = b2;
        node->field30 = c;
        if (FSi_SendCommand(node, 6, c, b2) == 0) {
            return 0;
        }
    }
    p = (unsigned int *)((char *)node + 0xc);
    *p |= 0x10;
    *p &= ~0x20;
    return 1;
}
