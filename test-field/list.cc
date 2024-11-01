#include <stdio.h>
#include <stdlib.h>

typedef struct ListNode {
    struct ListNode *prev, *next;
} ListNode;
void init_list_node(ListNode* node);
#define _insert_into_list(list, node) \
    (init_list_node(node), _merge_list(list, node))
ListNode* _merge_list(ListNode* node1, ListNode* node2);
ListNode* _detach_from_list(ListNode* node);

ListNode a, b;

ListNode pool[10];

#define for_list(node) for (ListNode* p = node.next; p != &node; p = p->next)

int main() {
    init_list_node(&a);
    init_list_node(&b);

    for (int i = 0; i < 5; ++i) {
        _insert_into_list(&a, &pool[i]);
    }
    for (int i = 5; i < 10; ++i) {
        _insert_into_list(&b, &pool[i]);
    }

    for_list(a) {
        printf("%04x ", (unsigned int)((int64_t)p & 0xFFFF));
    }
    printf("\n");

    for_list(b) {
        printf("%04x ", (unsigned int)((int64_t)p & 0xFFFF));
    }
    printf("\n");

    if (0){
        // does not keep order
        _merge_list(&a, _detach_from_list(&b));
    }

    if (0) {
        // if b empty, access null pointer
        _merge_list(&a, _detach_from_list(&b)->next);
    }

    _merge_list(&a, &b), _detach_from_list(&b);

    for_list(a) {
        printf("%04x ", (unsigned int)((int64_t)p & 0xFFFF));
    }
    printf("\n");

    return 0;
}

void init_list_node(ListNode* node) {
    node->prev = node;
    node->next = node;
}

ListNode* _merge_list(ListNode* node1, ListNode* node2) {
    if (!node1)
        return node2;
    if (!node2)
        return node1;

    // before: (arrow is the next pointer)
    //   ... --> node1 --> node3 --> ...
    //   ... <-- node2 <-- node4 <-- ...
    //
    // after:
    //   ... --> node1 --+  +-> node3 --> ...
    //                   |  |
    //   ... <-- node2 <-+  +-- node4 <-- ...

    ListNode* node3 = node1->next;
    ListNode* node4 = node2->prev;

    node1->next = node2;
    node2->prev = node1;
    node4->next = node3;
    node3->prev = node4;

    return node1;
}

// returning prev is a FAILURE
ListNode* _detach_from_list(ListNode* node) {
    ListNode* prev = node->prev;

    node->prev->next = node->next;
    node->next->prev = node->prev;
    init_list_node(node);

    if (prev == node)
        return NULL;
    return prev;
}