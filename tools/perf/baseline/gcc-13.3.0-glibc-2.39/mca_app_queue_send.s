mca_app_queue_send:
endbr64
cmp    $0x1f,%dl
ja     4c8 <mca_app_queue_send+0x12e>
push   %r15
push   %r14
push   %r13
push   %r12
push   %rbp
push   %rbx
mov    %rdi,%rbx
mov    %rsi,%r12
mov    %edx,%ebp
mov    %ecx,%r15d
mov    %r8,%r13
mov    %r9,%r14
lea    0x4(%rdi),%rdi
call   0 <nxt_app_nncq_dequeue>
mov    %eax,%esi
cmp    $0x20000,%eax
je     4d0 <mca_app_queue_send+0x136>
mov    %eax,%edx
lea    0x0(,%rdx,8),%rax
lea    (%rax,%rdx,1),%rcx
lea    (%rbx,%rcx,4),%rcx
mov    %bpl,0x100014(%rcx)
add    %rdx,%rax
lea    0x100015(%rbx,%rax,4),%rdx
movzbl %bpl,%eax
cmp    $0x7,%bpl
ja     437 <mca_app_queue_send+0x9d>
test   $0x4,%al
jne    426 <mca_app_queue_send+0x8c>
test   %eax,%eax
je     479 <mca_app_queue_send+0xdf>
movzbl (%r12),%ecx
mov    %cl,(%rdx)
test   $0x2,%al
je     479 <mca_app_queue_send+0xdf>
movzwl -0x2(%r12,%rax,1),%ecx
mov    %cx,-0x2(%rdx,%rax,1)
jmp    479 <mca_app_queue_send+0xdf>
mov    (%r12),%ecx
mov    %ecx,(%rdx)
mov    -0x4(%r12,%rax,1),%ecx
mov    %ecx,-0x4(%rdx,%rax,1)
jmp    479 <mca_app_queue_send+0xdf>
mov    (%r12),%rcx
mov    %rcx,(%rdx)
mov    -0x8(%r12,%rax,1),%rcx
mov    %rcx,-0x8(%rdx,%rax,1)
lea    0x8(%rdx),%rdi
and    $0xfffffffffffffff8,%rdi
sub    %rdi,%rdx
sub    %rdx,%r12
add    %edx,%eax
and    $0xfffffff8,%eax
cmp    $0x8,%eax
jb     479 <mca_app_queue_send+0xdf>
and    $0xfffffff8,%eax
mov    $0x0,%edx
mov    %edx,%ecx
mov    (%r12,%rcx,1),%r8
mov    %r8,(%rdi,%rcx,1)
add    $0x8,%edx
cmp    %eax,%edx
jb     468 <mca_app_queue_send+0xce>
mov    %esi,%eax
lea    (%rax,%rax,8),%rax
lea    (%rbx,%rax,4),%rax
mov    %r15d,0x100034(%rax)
mov    %esi,(%r14)
lea    0x8000c(%rbx),%rdi
call   3c <nxt_app_nncq_enqueue>
mov    $0x0,%eax
mov    $0x1,%edx
lock cmpxchg %edx,(%rbx)
sete   %al
mov    $0x0,%edx
test   %r13,%r13
je     4bb <mca_app_queue_send+0x121>
movzbl %al,%eax
mov    %eax,0x0(%r13)
mov    %edx,%eax
pop    %rbx
pop    %rbp
pop    %r12
pop    %r13
pop    %r14
pop    %r15
ret
mov    $0xffffffff,%edx
mov    %edx,%eax
ret
mov    $0xfffffffe,%edx
jmp    4bb <mca_app_queue_send+0x121>
