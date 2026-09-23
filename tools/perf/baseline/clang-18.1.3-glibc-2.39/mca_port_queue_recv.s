mca_port_queue_recv:
push   %rbp
push   %r14
push   %rbx
mov    %rdi,%rbx
jmp    147 <mca_port_queue_recv+0x17>
nopl   0x0(%rax)
inc    %ecx
cmp    %dx,%cx
je     180 <mca_port_queue_recv+0x50>
mov    0x1000c(%rbx),%eax
mov    %eax,%ecx
and    $0x3fff,%ecx
mov    0x10010(%rbx,%rcx,4),%ebp
mov    %ebp,%ecx
shr    $0xe,%ecx
mov    %eax,%edx
shr    $0xe,%edx
cmp    %dx,%cx
jne    140 <mca_port_queue_recv+0x10>
lea    0x1(%rax),%ecx
lock cmpxchg %ecx,0x1000c(%rbx)
jne    147 <mca_port_queue_recv+0x17>
and    $0x3fff,%ebp
jmp    185 <mca_port_queue_recv+0x55>
mov    $0x4000,%ebp
mov    $0xffffffffffffffff,%r14
cmp    $0x4000,%ebp
je     239 <mca_port_queue_recv+0x109>
mov    %ebp,%eax
mov    %ebp,%ecx
shl    $0x5,%ecx
movzbl 0x20014(%rbx,%rcx,1),%r14d
cmp    $0x20,%r14
jae    241 <mca_port_queue_recv+0x111>
shl    $0x5,%eax
add    %rbx,%rax
add    $0x20015,%rax
mov    %rsi,%rdi
mov    %rax,%rsi
mov    %r14,%rdx
call   1cc <mca_port_queue_recv+0x9c>
and    $0x3fff,%ebp
jmp    1ed <mca_port_queue_recv+0xbd>
data16 data16 cs nopw 0x0(%rax,%rax,1)
lea    0x1(%rcx),%edx
mov    %ecx,%eax
lock cmpxchg %edx,0x10008(%rbx)
mov    0x10008(%rbx),%ecx
mov    %ecx,%edx
and    $0x3fff,%edx
mov    %edx,%eax
mov    0x8(%rbx,%rax,4),%eax
mov    %eax,%esi
shr    $0xe,%esi
mov    %ecx,%edi
shr    $0xe,%edi
cmp    %di,%si
je     1e0 <mca_port_queue_recv+0xb0>
inc    %esi
cmp    %di,%si
jne    1ed <mca_port_queue_recv+0xbd>
mov    %ecx,%esi
and    $0x3fffc000,%esi
or     %ebp,%esi
lock cmpxchg %esi,0x8(%rbx,%rdx,4)
jne    1ed <mca_port_queue_recv+0xbd>
lea    0x1(%rcx),%edx
mov    %ecx,%eax
lock cmpxchg %edx,0x10008(%rbx)
lock decl (%rbx)
mov    %r14,%rax
pop    %rbx
pop    %r14
pop    %rbp
ret
mov    $0x1f,%r14d
jmp    1b2 <mca_port_queue_recv+0x82>
nopl   0x0(%rax)
