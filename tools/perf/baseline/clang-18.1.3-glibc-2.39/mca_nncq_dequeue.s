mca_nncq_dequeue:
jmp    517 <mca_nncq_dequeue+0x17>
data16 data16 data16 data16 cs nopw 0x0(%rax,%rax,1)
inc    %edx
cmp    %si,%dx
je     546 <mca_nncq_dequeue+0x46>
mov    (%rdi),%eax
mov    %eax,%ecx
and    $0x3fff,%ecx
mov    0x4(%rdi,%rcx,4),%ecx
mov    %ecx,%edx
shr    $0xe,%edx
mov    %eax,%esi
shr    $0xe,%esi
cmp    %si,%dx
jne    510 <mca_nncq_dequeue+0x10>
lea    0x1(%rax),%edx
lock cmpxchg %edx,(%rdi)
jne    517 <mca_nncq_dequeue+0x17>
and    $0x3fff,%ecx
mov    %ecx,%eax
ret
mov    $0x4000,%eax
ret
nopl   0x0(%rax)
