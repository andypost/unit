nxt_port_mmap_get_method:
endbr64
push   %rbx
test   %rdx,%rdx
je     <nxt_port_mmap_get_method+0xad>
mov    $0x0,%ebx
mov    $0x2,%ecx
jmp    <nxt_port_mmap_get_method+0x64>
mov    0x28(%rdx),%rsi
cmp    %rsi,0x30(%rdx)
jne    <nxt_port_mmap_get_method+0x76>
jmp    <nxt_port_mmap_get_method+0x5b>
mov    0x8(%rdi),%rcx
cmpl   $0x0,(%rcx)
jne    <nxt_port_mmap_get_method+0x33>
mov    %ebx,%eax
pop    %rbx
ret
lea    OFF(%rip),%rdx        # <rodata>
mov    %rcx,%rsi
mov    $0x1,%edi
mov    $0x0,%eax
call   *0x8(%rcx)
jmp    <nxt_port_mmap_get_method+0x2f>
cmp    $0x2,%ebx
je     <nxt_port_mmap_get_method+0x86>
test   %ebx,%ebx
mov    $0x1,%eax
cmove  %eax,%ebx
mov    0x18(%rdx),%rdx
test   %rdx,%rdx
je     <nxt_port_mmap_get_method+0x2f>
movzbl 0x25(%rdx),%eax
test   $0x1,%al
je     <nxt_port_mmap_get_method+0x1a>
mov    0x50(%rdx),%rsi
cmp    %rsi,0x58(%rdx)
je     <nxt_port_mmap_get_method+0x5b>
test   $0x4,%al
je     <nxt_port_mmap_get_method+0x4c>
cmp    $0x1,%ebx
je     <nxt_port_mmap_get_method+0x26>
test   %ebx,%ebx
cmove  %ecx,%ebx
jmp    <nxt_port_mmap_get_method+0x5b>
mov    0x8(%rdi),%rcx
mov    $0x1,%ebx
cmpl   $0x0,(%rcx)
je     <nxt_port_mmap_get_method+0x2f>
lea    OFF(%rip),%rdx        # <rodata>
mov    %rcx,%rsi
mov    $0x1,%edi
mov    $0x0,%eax
call   *0x8(%rcx)
jmp    <nxt_port_mmap_get_method+0x2f>
mov    $0x0,%ebx
jmp    <nxt_port_mmap_get_method+0x2f>
