mca_app_queue_send:
cmp    $0x1f,%dl
ja     39e <mca_app_queue_send+0x14e>
push   %rbp
push   %r15
push   %r14
push   %r13
push   %r12
push   %rbx
push   %rax
mov    %r9,%r15
mov    %r8,%rbx
mov    %ecx,%ebp
mov    %rdi,%r14
jmp    287 <mca_app_queue_send+0x37>
data16 data16 data16 data16 data16 cs nopw 0x0(%rax,%rax,1)
inc    %ecx
cmp    %di,%cx
je     2bb <mca_app_queue_send+0x6b>
mov    0x4(%r14),%eax
mov    %eax,%ecx
and    $0x1ffff,%ecx
mov    0x8(%r14,%rcx,4),%r12d
mov    %r12d,%ecx
shr    $0x11,%ecx
mov    %eax,%edi
shr    $0x11,%edi
cmp    %edi,%ecx
jne    280 <mca_app_queue_send+0x30>
lea    0x1(%rax),%ecx
lock cmpxchg %ecx,0x4(%r14)
jne    287 <mca_app_queue_send+0x37>
and    $0x1ffff,%r12d
jmp    2c1 <mca_app_queue_send+0x71>
mov    $0x20000,%r12d
mov    $0xfffffffe,%ecx
cmp    $0x20000,%r12d
je     38d <mca_app_queue_send+0x13d>
mov    %r12d,%eax
lea    (%rax,%rax,8),%r13
mov    %dl,0x100014(%r14,%r13,4)
lea    (%r14,%r13,4),%rdi
add    $0x100015,%rdi
movzbl %dl,%edx
call   2f5 <mca_app_queue_send+0xa5>
mov    %ebp,0x100034(%r14,%r13,4)
mov    %r12d,(%r15)
and    $0x1ffff,%r12d
jmp    31e <mca_app_queue_send+0xce>
nopl   0x0(%rax)
lea    0x1(%rcx),%edx
mov    %ecx,%eax
lock cmpxchg %edx,0x100010(%r14)
mov    0x100010(%r14),%ecx
mov    %ecx,%edx
and    $0x1ffff,%edx
mov    %edx,%eax
mov    0x80010(%r14,%rax,4),%eax
mov    %eax,%esi
shr    $0x11,%esi
mov    %ecx,%edi
shr    $0x11,%edi
cmp    %edi,%esi
je     310 <mca_app_queue_send+0xc0>
inc    %esi
cmp    %di,%si
jne    31e <mca_app_queue_send+0xce>
mov    %ecx,%esi
and    $0xfffe0000,%esi
or     %r12d,%esi
lock cmpxchg %esi,0x80010(%r14,%rdx,4)
jne    31e <mca_app_queue_send+0xce>
lea    0x1(%rcx),%edx
mov    %ecx,%eax
lock cmpxchg %edx,0x100010(%r14)
xor    %ecx,%ecx
mov    $0x1,%esi
xor    %edx,%edx
xor    %eax,%eax
lock cmpxchg %esi,(%r14)
sete   %al
test   %rbx,%rbx
je     38d <mca_app_queue_send+0x13d>
mov    %al,%dl
mov    %edx,(%rbx)
add    $0x8,%rsp
pop    %rbx
pop    %r12
pop    %r13
pop    %r14
pop    %r15
pop    %rbp
mov    %ecx,%eax
ret
mov    $0xffffffff,%eax
ret
data16 data16 cs nopw 0x0(%rax,%rax,1)
