mca_port_queue_send:
push   %rbp
push   %r14
push   %rbx
mov    %rcx,%rbx
xor    %ecx,%ecx
cmp    $0x1f,%dl
ja     11b <mca_port_queue_send+0x11b>
mov    %rdi,%r14
jmp    28 <mca_port_queue_send+0x28>
nopw   0x0(%rax,%rax,1)
inc    %edi
cmp    %r8w,%di
je     5e <mca_port_queue_send+0x5e>
mov    0x4(%r14),%eax
mov    %eax,%edi
and    $0x3fff,%edi
mov    0x8(%r14,%rdi,4),%ebp
mov    %ebp,%edi
shr    $0xe,%edi
mov    %eax,%r8d
shr    $0xe,%r8d
cmp    %r8w,%di
jne    20 <mca_port_queue_send+0x20>
lea    0x1(%rax),%edi
lock cmpxchg %edi,0x4(%r14)
jne    28 <mca_port_queue_send+0x28>
and    $0x3fff,%ebp
jmp    63 <mca_port_queue_send+0x63>
mov    $0x4000,%ebp
mov    $0xfffffffe,%eax
cmp    $0x4000,%ebp
je     114 <mca_port_queue_send+0x114>
mov    %ebp,%eax
shl    $0x5,%eax
mov    %dl,0x20014(%r14,%rax,1)
lea    (%r14,%rax,1),%rdi
add    $0x20015,%rdi
movzbl %dl,%edx
call   94 <mca_port_queue_send+0x94>
and    $0x3fff,%ebp
jmp    ae <mca_port_queue_send+0xae>
nopl   0x0(%rax)
lea    0x1(%rcx),%edx
mov    %ecx,%eax
lock cmpxchg %edx,0x20010(%r14)
mov    0x20010(%r14),%ecx
mov    %ecx,%edx
and    $0x3fff,%edx
mov    %edx,%eax
mov    0x10010(%r14,%rax,4),%eax
mov    %eax,%esi
shr    $0xe,%esi
mov    %ecx,%edi
shr    $0xe,%edi
cmp    %di,%si
je     a0 <mca_port_queue_send+0xa0>
inc    %esi
cmp    %di,%si
jne    ae <mca_port_queue_send+0xae>
mov    %ecx,%esi
and    $0x3fffc000,%esi
or     %ebp,%esi
lock cmpxchg %esi,0x10010(%r14,%rdx,4)
jne    ae <mca_port_queue_send+0xae>
lea    0x1(%rcx),%edx
mov    %ecx,%eax
lock cmpxchg %edx,0x20010(%r14)
mov    $0x1,%eax
lock xadd %eax,(%r14)
xor    %ecx,%ecx
test   %eax,%eax
sete   %cl
xor    %eax,%eax
mov    %ecx,(%rbx)
pop    %rbx
pop    %r14
pop    %rbp
ret
mov    $0xffffffff,%eax
jmp    114 <mca_port_queue_send+0x114>
data16 data16 data16 data16 cs nopw 0x0(%rax,%rax,1)
