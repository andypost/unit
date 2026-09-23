mca_nncq_dequeue:
endbr64
mov    (%rdi),%eax
mov    %eax,%edx
and    $0x3fff,%edx
mov    0x4(%rdi,%rdx,4),%ecx
mov    %ecx,%edx
shr    $0xe,%edx
mov    %eax,%esi
shr    $0xe,%esi
cmp    %dx,%si
je     636 <mca_nncq_dequeue+0x2f>
add    $0x1,%edx
cmp    %dx,%si
jne    60b <mca_nncq_dequeue+0x4>
mov    $0x4000,%eax
ret
lea    0x1(%rax),%edx
lock cmpxchg %edx,(%rdi)
jne    60b <mca_nncq_dequeue+0x4>
mov    %ecx,%eax
and    $0x3fff,%eax
ret
