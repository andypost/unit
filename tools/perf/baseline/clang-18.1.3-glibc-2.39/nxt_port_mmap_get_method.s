nxt_port_mmap_get_method:
test   %rdx,%rdx
je     <nxt_port_mmap_get_method+0x6c>
push   %rax
xor    %eax,%eax
mov    $0x2,%ecx
jmp    <nxt_port_mmap_get_method+0x19>
nop
mov    0x18(%rdx),%rdx
test   %rdx,%rdx
je     <nxt_port_mmap_get_method+0x94>
movzwl 0x25(%rdx),%esi
test   $0x1,%sil
jne    <nxt_port_mmap_get_method+0x40>
mov    0x30(%rdx),%r8
sub    0x28(%rdx),%r8
test   %r8,%r8
jne    <nxt_port_mmap_get_method+0x4d>
jmp    <nxt_port_mmap_get_method+0x10>
data16 data16 data16 data16 cs nopw 0x0(%rax,%rax,1)
mov    0x58(%rdx),%r8
sub    0x50(%rdx),%r8
test   %r8,%r8
je     <nxt_port_mmap_get_method+0x10>
test   $0x4,%sil
jne    <nxt_port_mmap_get_method+0x60>
cmp    $0x2,%eax
je     <nxt_port_mmap_get_method+0x86>
cmp    $0x1,%eax
adc    $0x0,%eax
jmp    <nxt_port_mmap_get_method+0x10>
cmp    $0x1,%eax
je     <nxt_port_mmap_get_method+0x6f>
test   %eax,%eax
cmove  %ecx,%eax
jmp    <nxt_port_mmap_get_method+0x10>
xor    %eax,%eax
ret
mov    0x8(%rdi),%rsi
mov    $0x1,%eax
cmpl   $0x0,(%rsi)
je     <nxt_port_mmap_get_method+0x94>
lea    OFF(%rip),%rdx        # <rodata>
jmp    <nxt_port_mmap_get_method+0xa0>
mov    0x8(%rdi),%rsi
mov    $0x1,%eax
cmpl   $0x0,(%rsi)
jne    <nxt_port_mmap_get_method+0x99>
add    $0x8,%rsp
ret
lea    OFF(%rip),%rdx        # <rodata>
mov    $0x1,%edi
xor    %eax,%eax
call   *0x8(%rsi)
mov    $0x1,%eax
add    $0x8,%rsp
ret
data16 data16 cs nopw 0x0(%rax,%rax,1)
