#include <atomic>
#include <thread>
#include <cassert>
#include <iostream>
#include </Users/stack_2/noncopyable.h>
using namespace std;


//////////////////////////////
// lock free lifo stack
template <class T>
class TLockFreeStack: TNonCopyable {
    struct TNode {
        T Value;
        TNode* Next;

        TNode() = default;

        template <class U>
        explicit TNode(U&& val)
                : Value(std::forward<U>(val))
                , Next(nullptr)
        {
        }
    };

    TNode* Head;
    TNode* FreePtr;
    atomic<int> DequeueCount;

    void TryToFreeMemory() {
        TNode* current = __atomic_load(FreePtr);
        if (!current)
            return;
        if (DequeueCount.fetch_add(0) == 1) {
            // node current is in free list, we are the last thread so try to cleanup
            if (__atomic_compare_exchange(&FreePtr, (TNode*)nullptr, current))
                EraseList(current);
        }
    }
    void EraseList(TNode* volatile p) {
        while (p) {
            TNode* next = p->Next;
            delete p;
            p = next;
        }
    }
    void EnqueueImpl(TNode* volatile head, TNode* volatile tail) {
        for (;;) {
            tail->Next = __atomic_load(Head, std::memory_order_relaxed);
            if (__atomic_compare_exchange(&Head, head, tail->Next))
                break;
        }    }
    template <class U>
    void EnqueueImpl(U&& u) {
        TNode* volatile node = new TNode(std::forward<U>(u));
        EnqueueImpl(node, node);
    }

public:
    TLockFreeStack()
            : Head(nullptr)
            , FreePtr(nullptr)
            , DequeueCount(0)
    {
    }
    ~TLockFreeStack() {
        EraseList(Head);
        EraseList(FreePtr);
    }

    void Enqueue(const T& t) {
        EnqueueImpl(t);
    }

    void Enqueue(T&& t) {
        EnqueueImpl(std::move(t));
    }

    template <typename TCollection>
    void EnqueueAll(const TCollection& data) {
        EnqueueAll(data.begin(), data.end());
    }
    template <typename TIter>
    void EnqueueAll(TIter dataBegin, TIter dataEnd) {
        if (dataBegin == dataEnd) {
            return;
        }
        TIter i = dataBegin;
        TNode* volatile node = new TNode(*i);
        TNode* volatile tail = node;

        for (++i; i != dataEnd; ++i) {
            TNode* nextNode = node;
            node = new TNode(*i);
            node->Next = nextNode;
        }
        EnqueueImpl(node, tail);
    }
    bool Dequeue(T* res) {
        DequeueCount.fetch_add(1);
        for (TNode* current = __atomic_load(Head, std::memory_order_relaxed); current; current = __atomic_load(Head)) {
            if (__atomic_(&Head,AtomicGet(current->Next), current)) {
                *res = std::move(current->Value);
                // delete current; // ABA problem
                // even more complex node deletion
                TryToFreeMemory();
                if (DequeueCount.fetch_add( -1) == 0) {
                    // no other Dequeue()s, can safely reclaim memory
                    delete current;
                } else {
                    // Dequeue()s in progress, put node to free list
                    for (;;) {
                        __atomic_store(current->Next,AtomicGet(FreePtr));
                        if (__atomic_compare_exchange(&FreePtr, current, current->Next))
                            break;
                    }
                }
                return true;
            }
        }
        TryToFreeMemory();
        DequeueCount.fetch_add(-1);
        return false;
    }
    // add all elements to *res
    // elements are returned in order of dequeue (top to bottom; see example in unittest)
    template <typename TCollection>
    void DequeueAll(TCollection* res) {
        DequeueCount.fetch_add( 1);
        for (TNode* current = __atomic_load(Head); current; current = __atomic_load(Head)) {
            if (__atomic_compare_exchange(&Head, (TNode*)nullptr, current)) {
                for (TNode* x = current; x;) {
                    res->push_back(std::move(x->Value));
                    x = x->Next;
                }
                // EraseList(current); // ABA problem
                // even more complex node deletion
                TryToFreeMemory();
                if (DequeueCount.fetch_add( -1) == 0) {
                    // no other Dequeue()s, can safely reclaim memory
                    EraseList(current);
                } else {
                    // Dequeue()s in progress, add nodes list to free list
                    TNode* currentLast = current;
                    while (currentLast->Next) {
                        currentLast = currentLast->Next;
                    }
                    for (;;) {
                        __atomic_store(currentLast->Next,AtomicGet(FreePtr));
                        if (__atomic_compare_exchange(&FreePtr, current, currentLast->Next))
                            break;
                    }
                }
                return;
            }
        }
        TryToFreeMemory();
        DequeueCount.fetch_add( -1);
    }
    bool DequeueSingleConsumer(T* res) {
        for (TNode* current = __atomic_load(Head); current; current = __atomic_load(Head)) {
            if (__atomic_compare_exchange(&Head, current->Next, current)) {
                *res = std::move(current->Value);
                delete current; // with single consumer thread ABA does not happen
                return true;
            }
        }
        return false;
    }
    // add all elements to *res
    // elements are returned in order of dequeue (top to bottom; see example in unittest)
    template <typename TCollection>
    void DequeueAllSingleConsumer(TCollection* res) {
        for (TNode* current = __atomic_load(Head); current; current = __atomic_load(Head)) {
            if (__atomic_compare_exchange(&Head, (TNode*)nullptr, current)) {
                for (TNode* x = current; x;) {
                    res->push_back(std::move(x->Value));
                    x = x->Next;
                }
                EraseList(current); // with single consumer thread ABA does not happen
                return;
            }
        }
    }
    bool IsEmpty() {
        DequeueCount.fetch_add(0);        // mem barrier
        return __atomic_load(Head) == nullptr; // without lock, so result is approximate
    }
};



// test

int main()
{
    TLockFreeStack<int> stack;
    std::thread th([&]() {
        int* x;
        for (int i = 0; i != 100; ++i)
            stack.Dequeue(x);
    });

    for (int i = 0; i != 100; ++i)
        stack.Enqueue(i);

    th.join();
}
// refference : https://clang.llvm.org/docs/ThreadSanitizer.html and https://stackoverflow.com/questions/37552866/why-does-threadsanitizer-report-a-race-with-this-lock-free-example