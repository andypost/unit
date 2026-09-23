mca_port_queue_send:
endbr64
mov    %rsi,%r8
mov    %edx,%esi
mov    %rcx,%r9
cmp    $0x1f,%dl
ja     21a <mca_port_queue_send+0x173>
mov    0x4(%rdi),%eax
mov    %eax,%edx
and    $0x3fff,%edx
mov    0x8(%rdi,%rdx,4),%r10d
mov    %r10d,%edx
shr    $0xe,%edx
mov    %eax,%ecx
shr    $0xe,%ecx
cmp    %dx,%cx
je     f3 <mca_port_queue_send+0x4c>
add    $0x1,%edx
cmp    %dx,%cx
jne    bc <mca_port_queue_send+0x15>
mov    $0x0,%edx
mov    $0xfffffffe,%eax
jmp    216 <mca_port_queue_send+0x16f>
lea    0x1(%rax),%edx
lock cmpxchg %edx,0x4(%rdi)
jne    bc <mca_port_queue_send+0x15>
and    $0x3fff,%r10d
mov    %r10d,%eax
shl    $0x5,%rax
lea    (%rdi,%rax,1),%rdx
mov    %sil,0x20014(%rdx)
lea    0x20015(%rdi,%rax,1),%rdx
movzbl %sil,%eax
cmp    $0x7,%sil
ja     157 <mca_port_queue_send+0xb0>
test   $0x4,%al
jne    147 <mca_port_queue_send+0xa0>
test   %eax,%eax
je     1a7 <mca_port_queue_send+0x100>
movzbl (%r8),%ecx
mov    %cl,(%rdx)
test   $0x2,%al
je     1a7 <mca_port_queue_send+0x100>
movzwl -0x2(%r8,%rax,1),%ecx
mov    %cx,-0x2(%rdx,%rax,1)
jmp    1a7 <mca_port_queue_send+0x100>
mov    (%r8),%ecx
mov    %ecx,(%rdx)
mov    -0x4(%r8,%rax,1),%ecx
mov    %ecx,-0x4(%rdx,%rax,1)
jmp    1a7 <mca_port_queue_send+0x100>
mov    (%r8),%rcx
mov    %rcx,(%rdx)
mov    -0x8(%r8,%rax,1),%rcx
mov    %rcx,-0x8(%rdx,%rax,1)
lea    0x8(%rdx),%rsi
and    $0xfffffffffffffff8,%rsi
sub    %rsi,%rdx
sub    %rdx,%r8
add    %edx,%eax
and    $0xfffffff8,%eax
cmp    $0x8,%eax
jb     1a7 <mca_port_queue_send+0x100>
and    $0xfffffff8,%eax
mov    $0x0,%edx
mov    %edx,%ecx
mov    (%r8,%rcx,1),%r11
mov    %r11,(%rsi,%rcx,1)
add    $0x8,%edx
cmp    %eax,%edx
jb     187 <mca_port_queue_send+0xe0>
jmp    1a7 <mca_port_queue_send+0x100>
lea    0x1(%rsi),%edx
mov    %esi,%eax
lock cmpxchg %edx,0x20010(%rdi)
mov    0x20010(%rdi),%esi
mov    %esi,%ecx
and    $0x3fff,%ecx
mov    %ecx,%eax
mov    0x10010(%rdi,%rax,4),%eax
mov    %eax,%r8d
shr    $0xe,%r8d
mov    %esi,%edx
shr    $0xe,%edx
cmp    %r8w,%dx
je     19a <mca_port_queue_send+0xf3>
add    $0x1,%r8d
cmp    %r8w,%dx
jne    1a7 <mca_port_queue_send+0x100>
add    $0x1,%ecx
mov    %ecx,%ecx
movzwl %dx,%edx
shl    $0xe,%edx
add    %r10d,%edx
lock cmpxchg %edx,0x1000c(%rdi,%rcx,4)
jne    1a7 <mca_port_queue_send+0x100>
lea    0x1(%rsi),%edx
mov    %esi,%eax
lock cmpxchg %edx,0x20010(%rdi)
mov    $0x1,%eax
lock xadd %eax,(%rdi)
test   %eax,%eax
sete   %dl
movzbl %dl,%edx
mov    $0x0,%eax
mov    %edx,(%r9)
ret
mov    $0x0,%edx
mov    $0xffffffff,%eax
jmp    216 <mca_port_queue_send+0x16f>
