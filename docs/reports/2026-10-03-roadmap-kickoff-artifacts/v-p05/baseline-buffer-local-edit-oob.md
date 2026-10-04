# Pre-P05 baseline defect: Buffer::apply_local_edit with mismatched cardinality

Baseline revision: 225ad23 (parent of P05)
Configuration: RelWithDebInfo, -DNDEBUG (assertions compiled out), ASan+UBSan

Two ranges and one replacement string. The only guard was
`assert(ranges.size() == new_text.size())`, so the edit walk then indexed `new_text[i]`
past the end of the vector.

```
=================================================================
==2433027==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x7bd7289e0cc8 at pc 0x55693605bbf4 bp 0x7fff09e58a20 sp 0x7fff09e58a10
READ of size 8 at 0x7bd7289e0cc8 thread T0
    #0 0x55693605bbf3 in std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >::empty() const /usr/include/c++/16/bits/basic_string.h:1356
    #1 0x55693605bbf3 in CollabText::Crdt::Buffer::apply_local_edit(std::vector<std::pair<unsigned int, unsigned int>, std::allocator<std::pair<unsigned int, unsigned int> > > const&, std::vector<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > > const&) /tmp/opencode/baseline-p05/libs/collabtext/src/crdt/Buffer.cpp:743
    #2 0x556935f95408 in main /tmp/opencode/baseline-p05/probe_ub.cpp:25
    #3 0x7fa72cc27780  (/usr/lib/libc.so.6+0x27780) (BuildId: 503200d7fda94a5dc6058d7e0694e5d1dcb2e372)
    #4 0x7fa72cc278b8 in __libc_start_main (/usr/lib/libc.so.6+0x278b8) (BuildId: 503200d7fda94a5dc6058d7e0694e5d1dcb2e372)
    #5 0x556935f94494 in _start (/tmp/opencode/baseline-p05/probe_ub+0x430494) (BuildId: a596df8b0fb8effec4d137f2134ec7470c4aba8a)

0x7bd7289e0cc8 is located 8 bytes before 32-byte region [0x7bd7289e0cd0,0x7bd7289e0cf0)
allocated by thread T0 here:
    #0 0x7fa72f72d2a1 in operator new(unsigned long) (/usr/lib/libasan.so.8+0x12d2a1) (BuildId: b8a4241051a1621937fdc46e867ba7ecb56d96ea)
    #1 0x556935fe9f4c in std::__new_allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > >::allocate(unsigned long, void const*) /usr/include/c++/16/bits/new_allocator.h:162
    #2 0x556935fe9f4c in std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > >::allocate(unsigned long) /usr/include/c++/16/bits/allocator.h:206
    #3 0x556935fe9f4c in std::allocator_traits<std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > >::allocate(std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > >&, unsigned long) /usr/include/c++/16/bits/alloc_traits.h:638
    #4 0x556935fe9f4c in std::_Vector_base<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > >::_M_allocate(unsigned long) /usr/include/c++/16/bits/stl_vector.h:389
    #5 0x556935fe9f4c in std::_Vector_base<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > >::_M_allocate(unsigned long) /usr/include/c++/16/bits/stl_vector.h:386
    #6 0x556935fe9f4c in std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >* std::vector<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > >::_M_allocate_and_copy<__gnu_cxx::__normal_iterator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const*, std::vector<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > > > >(unsigned long, __gnu_cxx::__normal_iterator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const*, std::vector<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > > >, __gnu_cxx::__normal_iterator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const*, std::vector<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > > >) /usr/include/c++/16/bits/stl_vector.h:1900
    #7 0x556935fe9f4c in std::vector<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > >::operator=(std::vector<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > > const&) /usr/include/c++/16/bits/vector.tcc:242
    #8 0x55693604d2fa in CollabText::Crdt::Buffer::apply_local_edit(std::vector<std::pair<unsigned int, unsigned int>, std::allocator<std::pair<unsigned int, unsigned int> > > const&, std::vector<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > > const&) /tmp/opencode/baseline-p05/libs/collabtext/src/crdt/Buffer.cpp:538
    #9 0x556935f95408 in main /tmp/opencode/baseline-p05/probe_ub.cpp:25
    #10 0x7fa72cc27780  (/usr/lib/libc.so.6+0x27780) (BuildId: 503200d7fda94a5dc6058d7e0694e5d1dcb2e372)
    #11 0x7fa72cc278b8 in __libc_start_main (/usr/lib/libc.so.6+0x278b8) (BuildId: 503200d7fda94a5dc6058d7e0694e5d1dcb2e372)
    #12 0x556935f94494 in _start (/tmp/opencode/baseline-p05/probe_ub+0x430494) (BuildId: a596df8b0fb8effec4d137f2134ec7470c4aba8a)

SUMMARY: AddressSanitizer: heap-buffer-overflow /tmp/opencode/baseline-p05/libs/collabtext/src/crdt/Buffer.cpp:743 in CollabText::Crdt::Buffer::apply_local_edit(std::vector<std::pair<unsigned int, unsigned int>, std::allocator<std::pair<unsigned int, unsigned int> > > const&, std::vector<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > > const&)
Shadow bytes around the buggy address:
  0x7bd7289e0a00: 00 00 01 fa fa fa 00 00 02 fa fa fa 00 00 01 fa
  0x7bd7289e0a80: fa fa 00 00 03 fa fa fa 00 00 00 01 fa fa 00 00
```
