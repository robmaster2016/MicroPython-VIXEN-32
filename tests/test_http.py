# DHCP, DNS and a plain HTTP request. Needs the internet.
import network, time, socket
lan = network.LAN()
lan.active(True)
t = time.ticks_ms()
while not lan.isconnected() and time.ticks_diff(time.ticks_ms(), t) < 15000:
    time.sleep_ms(50)
print("connected", lan.isconnected(), lan.ifconfig(), lan.status('stats'))
print("dns", socket.getaddrinfo("micropython.org", 80)[0][-1])
s = socket.socket()
s.connect(socket.getaddrinfo("micropython.org", 80)[0][-1])
s.write(b"GET / HTTP/1.0\r\nHost: micropython.org\r\n\r\n")
r = s.read(120)
s.close()
print(r)
assert lan.isconnected() and r.startswith(b"HTTP/1."), r
print("TEST OK")
