mca_port_mmap_get_free_chunk:
mov    (%rsi),%r8d
xor    %edx,%edx
cmp    $0x27f,%r8
ja     731 <mca_port_mmap_get_free_chunk+0x91>
push   %rbx
mov    $0xffffffffffffffff,%rax
mov    %r8d,%ecx
shl    %cl,%rax
shr    $0x6,%r8d
jmp    6e0 <mca_port_mmap_get_free_chunk+0x40>
cs nopw 0x0(%rax,%rax,1)
inc    %r8
mov    $0xffffffffffffffff,%rax
cmp    $0xa,%r8
je     730 <mca_port_mmap_get_free_chunk+0x90>
and    (%rdi,%r8,8),%rax
je     6d0 <mca_port_mmap_get_free_chunk+0x30>
tzcnt  %rax,%rcx
mov    $0x1,%r9d
shl    %cl,%r9
mov    $0xfffffffffffffffe,%r10
rol    %cl,%r10
xchg   %ax,%ax
mov    (%rdi,%r8,8),%r11
test   %r9,%r11
je     6d0 <mca_port_mmap_get_free_chunk+0x30>
mov    %r11,%rax
or     %r9,%rax
mov    %r11,%rbx
and    %r10,%rbx
lock cmpxchg %rbx,(%rdi,%r8,8)
jne    700 <mca_port_mmap_get_free_chunk+0x60>
test   %r9,%r11
je     6d0 <mca_port_mmap_get_free_chunk+0x30>
shl    $0x6,%r8d
or     %r8d,%ecx
mov    %ecx,(%rsi)
mov    $0x1,%edx
pop    %rbx
mov    %edx,%eax
ret
