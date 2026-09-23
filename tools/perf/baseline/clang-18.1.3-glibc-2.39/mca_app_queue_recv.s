mca_app_queue_recv:
push   %rbp
push   %r15
push   %r14
push   %rbx
push   %rax
mov    %rdx,%r15
mov    %rdi,%rbx
jmp    3cb <mca_app_queue_recv+0x1b>
nop
inc    %ecx
cmp    %dx,%cx
je     4a7 <mca_app_queue_recv+0xf7>
mov    0x8000c(%rbx),%eax
mov    %eax,%ecx
and    $0x1ffff,%ecx
mov    0x80010(%rbx,%rcx,4),%ebp
mov    %ebp,%ecx
shr    $0x11,%ecx
mov    %eax,%edx
shr    $0x11,%edx
cmp    %edx,%ecx
jne    3c0 <mca_app_queue_recv+0x10>
lea    0x1(%rax),%ecx
lock cmpxchg %ecx,0x8000c(%rbx)
jne    3cb <mca_app_queue_recv+0x1b>
and    $0x1ffff,%ebp
cmp    $0x20000,%ebp
je     4b8 <mca_app_queue_recv+0x108>
mov    %ebp,%eax
lea    (%rax,%rax,8),%rax
movzbl 0x100014(%rbx,%rax,4),%r14d
cmp    $0x20,%r14
jae    4d4 <mca_app_queue_recv+0x124>
lea    (%rbx,%rax,4),%rax
add    $0x100015,%rax
mov    %rsi,%rdi
mov    %rax,%rsi
mov    %r14,%rdx
call   43e <mca_app_queue_recv+0x8e>
mov    %ebp,(%r15)
and    $0x1ffff,%ebp
jmp    45d <mca_app_queue_recv+0xad>
nopl   0x0(%rax)
lea    0x1(%rcx),%edx
mov    %ecx,%eax
lock cmpxchg %edx,0x80008(%rbx)
mov    0x80008(%rbx),%ecx
mov    %ecx,%edx
and    $0x1ffff,%edx
mov    %edx,%eax
mov    0x8(%rbx,%rax,4),%eax
mov    %eax,%esi
shr    $0x11,%esi
mov    %ecx,%edi
shr    $0x11,%edi
cmp    %edi,%esi
je     450 <mca_app_queue_recv+0xa0>
inc    %esi
cmp    %di,%si
jne    45d <mca_app_queue_recv+0xad>
mov    %ecx,%esi
and    $0xfffe0000,%esi
or     %ebp,%esi
lock cmpxchg %esi,0x8(%rbx,%rdx,4)
jne    45d <mca_app_queue_recv+0xad>
lea    0x1(%rcx),%edx
mov    %ecx,%eax
lock cmpxchg %edx,0x80008(%rbx)
jmp    4c6 <mca_app_queue_recv+0x116>
mov    $0x20000,%ebp
cmp    $0x20000,%ebp
jne    40d <mca_app_queue_recv+0x5d>
movl   $0x0,(%r15)
mov    $0xffffffffffffffff,%r14
mov    %r14,%rax
add    $0x8,%rsp
pop    %rbx
pop    %r14
pop    %r15
pop    %rbp
ret
mov    $0x1f,%r14d
jmp    426 <mca_app_queue_recv+0x76>
nop
