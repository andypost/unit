mca_app_queue_recv:
endbr64
push   %r13
push   %r12
push   %rbp
push   %rbx
mov    %rdi,%rbp
mov    %rsi,%r12
mov    %rdx,%r13
lea    0x8000c(%rdi),%rdi
call   0 <nxt_app_nncq_dequeue>
cmp    $0x20000,%eax
je     560 <mca_app_queue_recv+0x89>
mov    %eax,%esi
mov    %eax,%ecx
lea    0x0(,%rcx,8),%rdx
lea    (%rdx,%rcx,1),%rax
lea    0x0(%rbp,%rax,4),%rax
movzbl 0x100014(%rax),%ebx
movzbl %bl,%ebx
mov    $0x1f,%eax
cmp    %rax,%rbx
cmova  %rax,%rbx
add    %rcx,%rdx
lea    0x100015(%rbp,%rdx,4),%rdx
cmp    $0x8,%ebx
jae    584 <mca_app_queue_recv+0xad>
test   $0x4,%bl
jne    571 <mca_app_queue_recv+0x9a>
test   %ebx,%ebx
je     5cc <mca_app_queue_recv+0xf5>
movzbl (%rdx),%ecx
mov    %cl,(%r12)
test   $0x2,%bl
je     5cc <mca_app_queue_recv+0xf5>
mov    %ebx,%eax
movzwl -0x2(%rdx,%rax,1),%edx
mov    %dx,-0x2(%r12,%rax,1)
jmp    5cc <mca_app_queue_recv+0xf5>
movl   $0x0,0x0(%r13)
mov    $0xffffffffffffffff,%rax
jmp    5dc <mca_app_queue_recv+0x105>
mov    (%rdx),%ecx
mov    %ecx,(%r12)
mov    %ebx,%eax
mov    -0x4(%rdx,%rax,1),%edx
mov    %edx,-0x4(%r12,%rax,1)
jmp    5cc <mca_app_queue_recv+0xf5>
mov    (%rdx),%rax
mov    %rax,(%r12)
mov    -0x8(%rbx,%rdx,1),%rax
mov    %rax,-0x8(%r12,%rbx,1)
lea    0x8(%r12),%r8
and    $0xfffffffffffffff8,%r8
sub    %r8,%r12
mov    %rdx,%rcx
sub    %r12,%rcx
lea    (%rbx,%r12,1),%eax
and    $0xfffffff8,%eax
cmp    $0x8,%eax
jb     5cc <mca_app_queue_recv+0xf5>
and    $0xfffffff8,%eax
mov    $0x0,%edx
mov    %edx,%edi
mov    (%rcx,%rdi,1),%r9
mov    %r9,(%r8,%rdi,1)
add    $0x8,%edx
cmp    %eax,%edx
jb     5bb <mca_app_queue_recv+0xe4>
mov    %esi,0x0(%r13)
lea    0x4(%rbp),%rdi
call   3c <nxt_app_nncq_enqueue>
mov    %rbx,%rax
pop    %rbx
pop    %rbp
pop    %r12
pop    %r13
ret
