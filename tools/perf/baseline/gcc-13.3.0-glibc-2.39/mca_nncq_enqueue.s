mca_nncq_enqueue:
endbr64
mov    %rdi,%rcx
and    $0x3fff,%esi
mov    %esi,%r9d
mov    0x10004(%rcx),%edi
mov    %edi,%esi
and    $0x3fff,%esi
mov    %esi,%eax
mov    0x4(%rcx,%rax,4),%eax
mov    %eax,%r8d
shr    $0xe,%r8d
mov    %edi,%edx
shr    $0xe,%edx
cmp    %r8w,%dx
je     6aa <mca_nncq_enqueue+0x63>
add    $0x1,%r8d
cmp    %r8w,%dx
jne    657 <mca_nncq_enqueue+0x10>
add    $0x1,%esi
mov    %esi,%esi
movzwl %dx,%edx
shl    $0xe,%edx
add    %r9d,%edx
lock cmpxchg %edx,(%rcx,%rsi,4)
jne    657 <mca_nncq_enqueue+0x10>
lea    0x1(%rdi),%edx
mov    %edi,%eax
lock cmpxchg %edx,0x10004(%rcx)
ret
lea    0x1(%rdi),%edx
mov    %edi,%eax
lock cmpxchg %edx,0x10004(%rcx)
jmp    657 <mca_nncq_enqueue+0x10>
