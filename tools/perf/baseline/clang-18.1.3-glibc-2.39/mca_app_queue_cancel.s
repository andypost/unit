mca_app_queue_cancel:
mov    %edx,%eax
mov    %esi,%ecx
lea    (%rcx,%rcx,8),%rdx
xor    %esi,%esi
xor    %ecx,%ecx
lock cmpxchg %esi,0x100034(%rdi,%rdx,4)
sete   %cl
mov    %ecx,%eax
ret
nopl   0x0(%rax,%rax,1)
