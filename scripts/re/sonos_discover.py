import socket
msg=('M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: "ssdp:discover"\r\nMX: 2\r\nST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n').encode()
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.settimeout(4);s.sendto(msg,('239.255.255.250',1900))
seen=set()
try:
 while True:
  data,addr=s.recvfrom(65535)
  if addr[0] in seen:continue
  seen.add(addr[0]);print('DEVICE',addr[0]);print(data.decode(errors='replace'))
except socket.timeout:pass
