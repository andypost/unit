mca_app_nncq_dequeue:
jmp    5e7 <mca_app_nncq_dequeue+0x17>
data16 data16 data16 data16 cs nopw 0x0(%rax,%rax,1)
inc    %edx
cmp    %si,%dx
je     615 <mca_app_nncq_dequeue+0x45>
mov    (%rdi),%eax
mov    %eax,%ecx
and    $0x1ffff,%ecx
mov    0x4(%rdi,%rcx,4),%ecx
mov    %ecx,%edx
shr    $0x11,%edx
mov    %eax,%esi
shr    $0x11,%esi
cmp    %esi,%edx
jne    5e0 <mca_app_nncq_dequeue+0x10>
lea    0x1(%rax),%edx
lock cmpxchg %edx,(%rdi)
jne    5e7 <mca_app_nncq_dequeue+0x17>
and    $0x1ffff,%ecx
mov    %ecx,%eax
ret
mov    $0x20000,%eax
ret
nopl   0x0(%rax,%rax,1)
