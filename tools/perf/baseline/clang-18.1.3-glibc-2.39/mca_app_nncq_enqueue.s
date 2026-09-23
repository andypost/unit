mca_app_nncq_enqueue:
and    $0x1ffff,%esi
jmp    63d <mca_app_nncq_enqueue+0x1d>
nopl   0x0(%rax,%rax,1)
lea    0x1(%rcx),%edx
mov    %ecx,%eax
lock cmpxchg %edx,0x80004(%rdi)
mov    0x80004(%rdi),%ecx
mov    %ecx,%edx
and    $0x1ffff,%edx
mov    %edx,%eax
mov    0x4(%rdi,%rax,4),%eax
mov    %eax,%r8d
shr    $0x11,%r8d
mov    %ecx,%r9d
shr    $0x11,%r9d
cmp    %r9d,%r8d
je     630 <mca_app_nncq_enqueue+0x10>
inc    %r8d
cmp    %r9w,%r8w
jne    63d <mca_app_nncq_enqueue+0x1d>
mov    %ecx,%r8d
and    $0xfffe0000,%r8d
or     %esi,%r8d
lock cmpxchg %r8d,0x4(%rdi,%rdx,4)
jne    63d <mca_app_nncq_enqueue+0x1d>
lea    0x1(%rcx),%edx
mov    %ecx,%eax
lock cmpxchg %edx,0x80004(%rdi)
ret
data16 data16 data16 data16 data16 cs nopw 0x0(%rax,%rax,1)
