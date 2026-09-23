mca_port_queue_recv:
endbr64
mov    %rdi,%rdx
mov    %rsi,%rdi
mov    0x1000c(%rdx),%eax
mov    %eax,%ecx
and    $0x3fff,%ecx
mov    0x10010(%rdx,%rcx,4),%r10d
mov    %r10d,%ecx
shr    $0xe,%ecx
mov    %eax,%esi
shr    $0xe,%esi
cmp    %cx,%si
je     266 <mca_port_queue_recv+0x40>
add    $0x1,%ecx
cmp    %cx,%si
jne    230 <mca_port_queue_recv+0xa>
mov    $0xffffffffffffffff,%rax
ret
lea    0x1(%rax),%ecx
lock cmpxchg %ecx,0x1000c(%rdx)
jne    230 <mca_port_queue_recv+0xa>
and    $0x3fff,%r10d
mov    %r10d,%esi
shl    $0x5,%rsi
lea    (%rdx,%rsi,1),%rax
movzbl 0x20014(%rax),%ecx
movzbl %cl,%ecx
mov    $0x1f,%eax
cmp    %rax,%rcx
cmova  %rax,%rcx
lea    0x20015(%rdx,%rsi,1),%rsi
cmp    $0x8,%ecx
jae    2e1 <mca_port_queue_recv+0xbb>
test   $0x4,%cl
jne    2cf <mca_port_queue_recv+0xa9>
test   %ecx,%ecx
je     337 <mca_port_queue_recv+0x111>
movzbl (%rsi),%r8d
mov    %r8b,(%rdi)
test   $0x2,%cl
je     337 <mca_port_queue_recv+0x111>
mov    %ecx,%eax
movzwl -0x2(%rsi,%rax,1),%esi
mov    %si,-0x2(%rdi,%rax,1)
jmp    337 <mca_port_queue_recv+0x111>
mov    (%rsi),%r8d
mov    %r8d,(%rdi)
mov    %ecx,%eax
mov    -0x4(%rsi,%rax,1),%esi
mov    %esi,-0x4(%rdi,%rax,1)
jmp    337 <mca_port_queue_recv+0x111>
mov    (%rsi),%rax
mov    %rax,(%rdi)
mov    -0x8(%rcx,%rsi,1),%rax
mov    %rax,-0x8(%rdi,%rcx,1)
lea    0x8(%rdi),%r9
and    $0xfffffffffffffff8,%r9
sub    %r9,%rdi
sub    %rdi,%rsi
mov    %rsi,%r8
lea    (%rcx,%rdi,1),%eax
and    $0xfffffff8,%eax
cmp    $0x8,%eax
jb     337 <mca_port_queue_recv+0x111>
and    $0xfffffff8,%eax
mov    $0x0,%esi
mov    %esi,%edi
mov    (%r8,%rdi,1),%r11
mov    %r11,(%r9,%rdi,1)
add    $0x8,%esi
cmp    %eax,%esi
jb     315 <mca_port_queue_recv+0xef>
jmp    337 <mca_port_queue_recv+0x111>
lea    0x1(%r8),%esi
mov    %r8d,%eax
lock cmpxchg %esi,0x10008(%rdx)
mov    0x10008(%rdx),%r8d
mov    %r8d,%edi
and    $0x3fff,%edi
mov    %edi,%eax
mov    0x8(%rdx,%rax,4),%eax
mov    %eax,%r9d
shr    $0xe,%r9d
mov    %r8d,%esi
shr    $0xe,%esi
cmp    %r9w,%si
je     328 <mca_port_queue_recv+0x102>
add    $0x1,%r9d
cmp    %r9w,%si
jne    337 <mca_port_queue_recv+0x111>
add    $0x1,%edi
mov    %edi,%edi
add    $0x1,%rdi
movzwl %si,%esi
shl    $0xe,%esi
add    %r10d,%esi
lock cmpxchg %esi,(%rdx,%rdi,4)
jne    337 <mca_port_queue_recv+0x111>
lea    0x1(%r8),%esi
mov    %r8d,%eax
lock cmpxchg %esi,0x10008(%rdx)
lock subl $0x1,(%rdx)
mov    %rcx,%rax
ret
