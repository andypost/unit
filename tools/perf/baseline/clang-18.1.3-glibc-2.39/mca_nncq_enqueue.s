mca_nncq_enqueue:
and    $0x3fff,%esi
jmp    56d <mca_nncq_enqueue+0x1d>
nopl   0x0(%rax,%rax,1)
lea    0x1(%rcx),%edx
mov    %ecx,%eax
lock cmpxchg %edx,0x10004(%rdi)
mov    0x10004(%rdi),%ecx
mov    %ecx,%edx
and    $0x3fff,%edx
mov    %edx,%eax
mov    0x4(%rdi,%rax,4),%eax
mov    %eax,%r8d
shr    $0xe,%r8d
mov    %ecx,%r9d
shr    $0xe,%r9d
cmp    %r9w,%r8w
je     560 <mca_nncq_enqueue+0x10>
inc    %r8d
cmp    %r9w,%r8w
jne    56d <mca_nncq_enqueue+0x1d>
mov    %ecx,%r8d
and    $0x3fffc000,%r8d
or     %esi,%r8d
lock cmpxchg %r8d,0x4(%rdi,%rdx,4)
jne    56d <mca_nncq_enqueue+0x1d>
lea    0x1(%rcx),%edx
mov    %ecx,%eax
lock cmpxchg %edx,0x10004(%rdi)
ret
data16 data16 data16 data16 cs nopw 0x0(%rax,%rax,1)
