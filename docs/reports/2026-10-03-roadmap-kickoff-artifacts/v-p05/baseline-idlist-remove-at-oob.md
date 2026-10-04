# Pre-P05 baseline defects, reproduced with AddressSanitizer + UBSan

Baseline revision: 225ad23 (parent of P05)
Configuration: RelWithDebInfo, -DNDEBUG (assertions compiled out, as in any Release build), ASan+UBSan

## Case 1: IdList::remove_at with an anchor that names no entry

The only guard was `assert(idx < entries.size())`. With assertions compiled out, `entries[idx]`
reads one element past the end of the vector.

```
=================================================================
==2433008==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x7c358bfe0298 at pc 0x55559f9cab77 bp 0x7ffe3f5e7dc0 sp 0x7ffe3f5e7db0
READ of size 8 at 0x7c358bfe0298 thread T0
    #0 0x55559f9cab76 in CollabText::Crdt::IdList::remove_at(CollabText::Crdt::Anchor const&) /tmp/opencode/baseline-p05/libs/collabtext/src/crdt/IdList.cpp:99
    #1 0x55559f7c4c53 in main /tmp/opencode/baseline-p05/probe_ub.cpp:14
    #2 0x7fc590227780  (/usr/lib/libc.so.6+0x27780) (BuildId: 503200d7fda94a5dc6058d7e0694e5d1dcb2e372)
    #3 0x7fc5902278b8 in __libc_start_main (/usr/lib/libc.so.6+0x278b8) (BuildId: 503200d7fda94a5dc6058d7e0694e5d1dcb2e372)
    #4 0x55559f7c4494 in _start (/tmp/opencode/baseline-p05/probe_ub+0x430494) (BuildId: a596df8b0fb8effec4d137f2134ec7470c4aba8a)

0x7c358bfe0298 is located 0 bytes after 72-byte region [0x7c358bfe0250,0x7c358bfe0298)
allocated by thread T0 here:
    #0 0x7fc592d2d2a1 in operator new(unsigned long) (/usr/lib/libasan.so.8+0x12d2a1) (BuildId: b8a4241051a1621937fdc46e867ba7ecb56d96ea)
    #1 0x55559f9fa5dd in std::__new_allocator<CollabText::Crdt::IdListEntry>::allocate(unsigned long, void const*) /usr/include/c++/16/bits/new_allocator.h:162
    #2 0x55559f9fa5dd in std::allocator<CollabText::Crdt::IdListEntry>::allocate(unsigned long) /usr/include/c++/16/bits/allocator.h:206
    #3 0x55559f9fa5dd in std::allocator_traits<std::allocator<CollabText::Crdt::IdListEntry> >::allocate(std::allocator<CollabText::Crdt::IdListEntry>&, unsigned long) /usr/include/c++/16/bits/alloc_traits.h:638
    #4 0x55559f9fa5dd in std::_Vector_base<CollabText::Crdt::IdListEntry, std::allocator<CollabText::Crdt::IdListEntry> >::_M_allocate(unsigned long) /usr/include/c++/16/bits/stl_vector.h:389
    #5 0x55559f9fa5dd in std::_Vector_base<CollabText::Crdt::IdListEntry, std::allocator<CollabText::Crdt::IdListEntry> >::_M_allocate(unsigned long) /usr/include/c++/16/bits/stl_vector.h:386
    #6 0x55559f9fa5dd in void std::vector<CollabText::Crdt::IdListEntry, std::allocator<CollabText::Crdt::IdListEntry> >::_M_realloc_append<CollabText::Crdt::IdListEntry const&>(CollabText::Crdt::IdListEntry const&) /usr/include/c++/16/bits/vector.tcc:580
    #7 0x55559f9fbec0 in std::vector<CollabText::Crdt::IdListEntry, std::allocator<CollabText::Crdt::IdListEntry> >::push_back(CollabText::Crdt::IdListEntry const&) /usr/include/c++/16/bits/stl_vector.h:1417
    #8 0x55559f9fbec0 in CollabText::Crdt::SumTree<CollabText::Crdt::IdListEntry, 6ul>::items() const::{lambda(CollabText::Crdt::IdListEntry const&)#1}::operator()(CollabText::Crdt::IdListEntry const&) const /tmp/opencode/baseline-p05/libs/collabtext/src/crdt/SumTree.h:453
    #9 0x55559f9fbec0 in void CollabText::Crdt::SumTree<CollabText::Crdt::IdListEntry, 6ul>::for_each_node<CollabText::Crdt::SumTree<CollabText::Crdt::IdListEntry, 6ul>::items() const::{lambda(CollabText::Crdt::IdListEntry const&)#1}>(CollabText::Crdt::SumTree<CollabText::Crdt::IdListEntry, 6ul>::Node const*, CollabText::Crdt::SumTree<CollabText::Crdt::IdListEntry, 6ul>::items() const::{lambda(CollabText::Crdt::IdListEntry const&)#1}&) /tmp/opencode/baseline-p05/libs/collabtext/src/crdt/SumTree.h:885
    #10 0x55559f9c255a in void CollabText::Crdt::SumTree<CollabText::Crdt::IdListEntry, 6ul>::for_each<CollabText::Crdt::SumTree<CollabText::Crdt::IdListEntry, 6ul>::items() const::{lambda(CollabText::Crdt::IdListEntry const&)#1}>(CollabText::Crdt::SumTree<CollabText::Crdt::IdListEntry, 6ul>::items() const::{lambda(CollabText::Crdt::IdListEntry const&)#1}&&) const /tmp/opencode/baseline-p05/libs/collabtext/src/crdt/SumTree.h:447
    #11 0x55559f9c255a in CollabText::Crdt::SumTree<CollabText::Crdt::IdListEntry, 6ul>::items() const /tmp/opencode/baseline-p05/libs/collabtext/src/crdt/SumTree.h:453
    #12 0x55559f9c255a in CollabText::Crdt::IdList::entries() const /tmp/opencode/baseline-p05/libs/collabtext/src/crdt/IdList.cpp:321
    #13 0x55559f9c299d in CollabText::Crdt::IdList::get_entries() const (/tmp/opencode/baseline-p05/probe_ub+0x62e99d) (BuildId: a596df8b0fb8effec4d137f2134ec7470c4aba8a)
    #14 0x55559f9c90d8 in CollabText::Crdt::IdList::remove_at(CollabText::Crdt::Anchor const&) /tmp/opencode/baseline-p05/libs/collabtext/src/crdt/IdList.cpp:85
    #15 0x55559f7c4c53 in main /tmp/opencode/baseline-p05/probe_ub.cpp:14
    #16 0x7fc590227780  (/usr/lib/libc.so.6+0x27780) (BuildId: 503200d7fda94a5dc6058d7e0694e5d1dcb2e372)
    #17 0x7fc5902278b8 in __libc_start_main (/usr/lib/libc.so.6+0x278b8) (BuildId: 503200d7fda94a5dc6058d7e0694e5d1dcb2e372)
    #18 0x55559f7c4494 in _start (/tmp/opencode/baseline-p05/probe_ub+0x430494) (BuildId: a596df8b0fb8effec4d137f2134ec7470c4aba8a)
```
