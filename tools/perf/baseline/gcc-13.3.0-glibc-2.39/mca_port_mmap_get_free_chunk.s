mca_port_mmap_get_free_chunk:
endbr64
mov    %rsi,%r11
mov    (%rsi),%ecx
mov    %ecx,%esi
shr    $0x6,%esi
mov    $0xffffffffffffffff,%rax
shl    %cl,%rax
cmp    $0x9,%esi
ja     75d <mca_port_mmap_get_free_chunk+0x90>
push   %rbp
push   %rbx
mov    %esi,%edx
mov    $0x1,%ebx
mov    $0xffffffffffffffff,%r10
jmp    709 <mca_port_mmap_get_free_chunk+0x3c>
add    $0x1,%rdx
mov    %r10,%rax
cmp    $0xa,%rdx
je     755 <mca_port_mmap_get_free_chunk+0x88>
and    (%rdi,%rdx,8),%rax
je     6fc <mca_port_mmap_get_free_chunk+0x2f>
tzcnt  %rax,%rax
mov    %edx,%ecx
shl    $0x6,%ecx
lea    (%rcx,%rax,1),%ecx
mov    %ecx,%eax
shr    $0x6,%eax
mov    %eax,%eax
lea    (%rdi,%rax,8),%r8
mov    %rbx,%rsi
shl    %cl,%rsi
mov    (%r8),%r9
test   %r9,%rsi
je     6fc <mca_port_mmap_get_free_chunk+0x2f>
mov    %rsi,%rax
or     %r9,%rax
mov    %rsi,%rbp
not    %rbp
and    %rbp,%r9
lock cmpxchg %r9,(%r8)
jne    72d <mca_port_mmap_get_free_chunk+0x60>
mov    %ecx,(%r11)
mov    $0x1,%eax
jmp    75a <mca_port_mmap_get_free_chunk+0x8d>
mov    $0x0,%eax
pop    %rbx
pop    %rbp
ret
mov    $0x0,%eax
ret
