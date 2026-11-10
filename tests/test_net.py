# net.up(): DHCP, then a static setup from a config file; TCP over the
# emulated Ethernet. Needs the internet (QEMU user networking).
import net, vfs, io
lan = net.up()
lan.active(False)
print("off", lan.active(), lan.status())
# static setup from a config file
class F:
    def __init__(s, t): s.t = t
    def mount(s, *a): pass
    def stat(s, p): return (0x8000,0,0,0,0,0,len(s.t),0,0,0)
    def open(s, p, m): return io.StringIO(s.t)
vfs.mount(F("#ip: dhcp\nip:   10.0.2.77\nmask: 255.255.255.0\ngw: 10.0.2.2\ndns: 10.0.2.3\nmac: 02:11:22:33:44:55\n"), "/c")
lan = net.up(path="/c/net.txt")
assert lan.ifconfig()[0] == "10.0.2.77", lan.ifconfig()
import socket
s = socket.socket(); s.connect(socket.getaddrinfo("example.com", 80)[0][-1])
s.write(b"HEAD / HTTP/1.0\r\nHost: example.com\r\n\r\n"); r = s.read(15); s.close(); print(r)
assert r.startswith(b"HTTP/1."), r
print("TEST OK")
