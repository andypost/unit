mca_app_queue_cancel:
endbr64
mov    %edx,%eax
mov    %esi,%esi
lea    (%rsi,%rsi,8),%rdx
lea    0x100034(%rdi,%rdx,4),%rdx
mov    $0x0,%ecx
lock cmpxchg %ecx,(%rdx)
sete   %al
movzbl %al,%eax
ret
