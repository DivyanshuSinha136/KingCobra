#include "ast.h"

void kc_list_push(KcNodeList *l, KcNode *n) {
    if (l->count + 1 > l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4;
        l->items = kc_realloc(l->items, sizeof(KcNode *) * l->cap);
    }
    l->items[l->count++] = n;
}

KcNode *kc_node_new(KcNodeKind kind, KcPos pos) {
    KcNode *n = kc_malloc(sizeof(KcNode));
    memset(n, 0, sizeof(KcNode));
    n->kind = kind;
    n->pos = pos;
    return n;
}
